/* The pinned vendor HAL embeds libwayland-client. Resolve its entry points;
 * all protocol objects and the ordinary HWC attach use that same connection. */
#include "wayland-api.h"
#include <wayland-client.h>
#include "wd-client.h"
#include "ticket.h"
#include <sys/time.h>

static struct wd_texture_manager_v1 *manager;
static struct wl_event_queue *queue;
static int accepted;
static uint32_t offered;
static unsigned in_flight;
static struct wl_buffer *cached;
static uint64_t cached_id, cached_nonce;
static bool used_in_set;
static __typeof__(((hwc_composer_device_1_t*)0)->set) real_set;
static bool available, checked;
#include "async-target.h"
static void on_global(void *data,struct wl_registry *registry,uint32_t name,const char *interface,uint32_t version)
{
    if(!manager && !strcmp(interface,"wd_texture_manager_v1")) {
        if(version>=3) bind_async_manager(registry,name);
        manager=wl_registry_bind(registry,name,&wd_texture_manager_v1_interface,version>=3 ? 3 : version);
    }
}
static void removed(void *data,struct wl_registry *registry,uint32_t name) {}
static const struct wl_registry_listener registry_listener={on_global,removed};
static void result(void *data,struct wd_texture_manager_v1 *object,uint32_t serial,uint32_t ok)
{
    accepted=(serial==offered && ok);
}
static const struct wd_texture_manager_v1_listener manager_listener={result};
static void released(void *data,struct wl_buffer *b)
{
    wl_buffer_destroy(b);
    if(in_flight) --in_flight;
}
static const struct wl_buffer_listener release_listener={released};

static bool weston_texture_bind_display(struct wl_display *d)
{
    if(checked) return available;
    checked=true;
    queue=wl_display_create_queue(d);
    if(!queue) return false;
    struct wl_display *wrap=wl_proxy_create_wrapper(d);
    if(!wrap) return false;
    wl_proxy_set_queue((struct wl_proxy *)wrap,queue);
    struct wl_registry *reg=wl_display_get_registry(wrap);
    wl_proxy_wrapper_destroy(wrap);
    if(!reg) return false;
    wl_registry_add_listener(reg,&registry_listener,NULL);
    int rc=wl_display_roundtrip_queue(d,queue);
    wl_registry_destroy(reg);
    if(rc<0 || !manager) return false;
    wd_texture_manager_v1_add_listener(manager,&manager_listener,NULL);
    available=true;
    LOG("weston-texture Weston texture protocol ready");
    return true;
}
static int weston_texture_get_texture(const struct wbuffer *b,uint64_t id,struct weston_texture_ticket mark,uint32_t *opaque)
{
    int sock=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
    if(sock<0) return -1;
    int texture=-1;
    struct sockaddr_un addr={.sun_family=AF_UNIX,.sun_path=FRAME_SOCKET};
    struct timeval timeout={.tv_usec=100000};
    struct ucred peer;
    socklen_t len=sizeof(peer);
    if(connect(sock,(void*)&addr,sizeof(addr)) || getsockopt(sock,SOL_SOCKET,SO_PEERCRED,&peer,&len) ||
       peer.uid!=1000 || fcntl(sock,F_SETFL,0) || setsockopt(sock,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))) goto out;
    struct frame_frame p={.magic=FRAME_MAGIC,.phase=WESTON_TEXTURE_FETCH,.buffer_id=id,.serial=mark.serial,
                       .alpha_fix=mark.nonce>>32,.sample=(uint32_t)mark.nonce};
    int plane=b->handle->data[0], received=-1;
    if(frame_packet(sock,&p,&plane,1,1) && frame_packet(sock,&p,&received,1,0)) {
        if(p.phase==FRAME_DONE && p.buffer_id==id && p.serial==mark.serial && p.nonempty<=1) {
            texture=received;
            *opaque=p.nonempty;
        } else close(received);
    }
out:
    close(sock);
    return texture;
}
static bool weston_texture_present(void *display,struct wbuffer *b,const void *pixels)
{
    if(b->width!=1222 || !frame_full_height(b->height) || b->stride<1226 || !b->handle || b->handle->numFds<1) return false;
    struct weston_texture_ticket mark;
    memcpy(&mark,(const uint8_t*)pixels+1222*4,sizeof(mark));
    if(mark.magic!=WESTON_TEXTURE_MAGIC || !mark.nonce) return false;
    struct vendor_display {pthread_t thread; struct wl_display *connection;};
    struct wl_display *d=((struct vendor_display*)display)->connection;
    if(!weston_texture_bind_display(d)) return false;
    if(wl_display_dispatch_queue_pending(d,queue)<0 || in_flight>=16) return false;
    uint64_t id=0;
    if(mapper_id(*mapper,b->handle,&id)) return false;
    uint32_t opaque;
    int fd=weston_texture_get_texture(b,id,mark,&opaque);
    if(fd<0) return false;
    if(cached && cached_id==id && cached_nonce==mark.nonce) {
        close(fd);
        wd_texture_manager_v1_reuse(manager,cached,b->wl_buffer);
        used_in_set=true;
        return true;
    }
    accepted=false;
    offered=mark.serial;
    struct wl_buffer *gpu=wd_texture_manager_v1_offer(manager,b->wl_buffer,fd,opaque,offered);
    close(fd);
    if(!gpu) return false;
    wl_buffer_add_listener(gpu,&release_listener,NULL);
    if(wl_display_roundtrip_queue(d,queue)<0 || !accepted) {
        wl_buffer_destroy(gpu);
        return false;
    }
    ++in_flight;
    if(cached) wd_texture_manager_v1_retire(manager,cached);
    cached=gpu; cached_id=id; cached_nonce=mark.nonce; used_in_set=true;
    return true;
}
#include "client-hwc.h"
static int weston_texture_set(hwc_composer_device_1_t *dev,size_t count,hwc_display_contents_1_t **lists)
{
    used_in_set=false;
    client_target_prepare_client((struct waydroid_device *)dev,count,lists);
    int ret=real_set(dev,count,lists);
    client_target_end_set();
    if(manager) wd_texture_manager_v1_finish(manager);
    if(cached && !used_in_set) {
        wd_texture_manager_v1_retire(manager,cached);
        cached=NULL;
    }
    return ret;
}
static void weston_texture_install(void *lib,struct waydroid_device *dev)
{
    if(!weston_texture_load_client(lib)) {checked=true; return;}
    real_set=dev->base.set;
    dev->base.set=weston_texture_set;
}
