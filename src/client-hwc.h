#include "transport.h"

static struct wl_buffer *physical_gpu;
static uint64_t cached_physical_id;
static uint32_t cached_generation,cached_process;
static __thread buffer_handle_t physical_handle;
static __thread uint64_t physical_id;
static __thread struct frame_frame frame;
static __thread bool physical_prepared, physical_used, fence_failed, mirror_ready;

static uint32_t proven_width, proven_height, proven_process;
static __thread bool target_attached, recovery_frame;

static int request_physical(uint32_t phase, int *texture)
{
    int sd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);
    if(sd<0) return 0;
    struct sockaddr_un addr;
    socklen_t size=physical_address(&addr);
    struct frame_frame p={.magic=FRAME_MAGIC,.buffer_id=physical_id,.phase=phase};
    int plane=physical_handle->data[0],rc=0;
    physical_timeout(sd);
    int connected=connect(sd,(void*)&addr,size);
    if(connected && (errno==ECONNREFUSED || errno==ENOENT)) rc=-ENOENT;
    if(!connected && physical_peer(sd,"init.svc_debug_pid.surfaceflinger") &&
       frame_packet(sd,&p,&plane,1,1) && recv(sd,&p,sizeof(p),MSG_PEEK)==sizeof(p)) {
        int gpu=p.phase==PHYSICAL_GPU && phase==PHYSICAL_GET;
        int fd=-1;
        if(frame_packet(sd,&p,&fd,gpu,0)) {
            if(p.buffer_id==physical_id && (gpu || p.phase==PHYSICAL_MIRROR)) {
                if(gpu) {frame=p; *texture=fd;}
                rc=p.phase;
            } else if(fd>=0) close(fd);
        }
    }
    close(sd);
    return rc;
}

static void client_target_prepare_client(struct waydroid_device *dev,size_t count,hwc_display_contents_1_t **lists)
{
    physical_used=physical_prepared=fence_failed=mirror_ready=false;
    physical_handle=NULL;
    target_attached=recovery_frame=false;
    if(!count || !lists || !lists[0] || !mapper || !*mapper) return;
    hwc_display_contents_1_t *d=lists[0];
    hwc_layer_1_t *target=NULL;
    bool has_client=false;
    for(size_t i=0;i<d->numHwLayers;i++) {
        hwc_layer_1_t *layer=&d->hwLayers[i];
        if(layer->compositionType==HWC_FRAMEBUFFER_TARGET) target=layer;
        else if(layer->compositionType==HWC_FRAMEBUFFER || (layer->flags&HWC_SKIP_LAYER)) has_client=true;
    }
    uint64_t usage=0;
    if(!has_client || !target || !target->handle || target->handle->numFds<1 ||
       !get_usage || get_usage(*mapper,target->handle,&usage) || usage!=0x1b00 ||
       mapper_id(*mapper,target->handle,&physical_id)) return;
    physical_handle=target->handle;
    struct pollfd fence={.fd=target->acquireFenceFd,.events=POLLIN};
    if(fence.fd>=0) {
        int rc;
        do {rc=poll(&fence,1,3000);} while(rc<0 && errno==EINTR);
        if(rc!=1 || !(fence.revents&POLLIN)) {fence_failed=true; return;}
    }
    struct vendor_display {pthread_t thread; struct wl_display *connection;};
    struct wl_display *wl=((struct vendor_display *)dev->display)->connection;
    if(!weston_texture_bind_display(wl) || wl_proxy_get_version((struct wl_proxy*)manager)<3 ||
       wl_display_dispatch_queue_pending(wl,queue)<0 || in_flight>=16) return;
    if ((recovery_frame=take_mirror_request())) return;
    int fd=-1;
    int reply=request_physical(PHYSICAL_GET,&fd);
    mirror_ready=reply==PHYSICAL_MIRROR;
    if(reply!=PHYSICAL_GPU) return;
    bool same_buffer=physical_gpu && cached_physical_id==physical_id &&
                     cached_generation==frame.serial && cached_process==frame.sample;
    if(!same_buffer && proven_width==frame.width && proven_height==frame.height &&
       proven_process==frame.sample && start_offer(dev->procs,fd,&frame)) {
        close(fd);
        physical_prepared=true;
        return;
    }
    if(!same_buffer) {
        accepted=false;
        offered=frame.serial;
        struct wl_buffer *buf=wd_texture_manager_v1_prepare_target(manager,fd,frame.width,frame.height,offered);
        close(fd);
        if(!buf) return;
        wl_buffer_add_listener(buf,&release_listener,NULL);
        if(wl_display_roundtrip_queue(wl,queue)<0 || !accepted) {wl_buffer_destroy(buf); return;}
        if(physical_gpu) wd_texture_manager_v1_retire(manager,physical_gpu);
        ++in_flight;
        physical_gpu=buf;
        cached_physical_id=physical_id;
        cached_generation=frame.serial;
        cached_process=frame.sample;
    } else close(fd);
    physical_prepared=true;
}
static bool client_target_attach_client(struct wbuffer *b)
{
    if(!physical_handle || b->handle!=physical_handle) return false;
    if(physical_prepared && b->is_shm && b->wl_buffer && b->format==1 &&
       b->width==frame.width && b->height==frame.height && b->stride==frame.stride) {
        if(submitting) {
            if(attach_offer(b->wl_buffer)) {target_attached=true; return true;}
            recovery_frame=true;
        } else {
            target_attached=true;
            proven_width=frame.width; proven_height=frame.height; proven_process=frame.sample;
            wd_texture_manager_v1_reuse(manager,physical_gpu,b->wl_buffer);
            physical_used=true;
            return true;
        }
    }
    int rc=mirror_ready ? PHYSICAL_MIRROR : (fence_failed ? 0 : request_physical(PHYSICAL_CPU,NULL));
    if(recovery_frame) LOG("target-import recovery id=%llu reply=%d",(unsigned long long)physical_id,rc);
    if(rc!=PHYSICAL_MIRROR && rc!=-ENOENT && manager && wl_proxy_get_version((struct wl_proxy*)manager)>=3) {
        wd_texture_manager_v1_hold_carrier(manager,b->wl_buffer);
        LOG("physical retained surface after recovery failure");
        return true;
    }
    return false;
}
static void client_target_end_set(void)
{
    end_offer();
    if(!target_attached) proven_process=0;
    physical_handle=NULL;
    if(physical_gpu && !physical_used) {
        wd_texture_manager_v1_retire(manager,physical_gpu);
        physical_gpu=NULL;
    }
}
