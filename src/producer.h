#include "transport.h"
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/eventfd.h>
#include <time.h>
#include <linux/dma-buf.h>
#include <sys/system_properties.h>

static int frame_surfaceflinger(void);
static _Atomic int client_target_outputs;
struct physical_slot {
    uint64_t id;
    dev_t dev;
    ino_t inode;
    int plane, width, height, stride, users, mirrored, direct;
    size_t bytes;
    void *pixels, *resource;
    int texture, ready;
    uint32_t generation;
};
static struct physical_slot slots[16];
static pthread_mutex_t slots_lock=PTHREAD_MUTEX_INITIALIZER;
static int listening, retry_wake = -1;
static unsigned generation;
#include <sys/system_properties.h>
static int texture_cache_flag(const char *name, int default_value)
{
    char value[PROP_VALUE_MAX];
    return __system_property_get(name,value) ? value[0]=='1' : default_value;
}
static int64_t texture_cache_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC,&ts);
    return (int64_t)ts.tv_sec*1000000000 + ts.tv_nsec;
}
static int (*export_target)(int,int,int *,void **,int,int64_t *);
static int (*export_snapshot)(int,int,int *,void **);
static void (*drop_snapshot)(void *);
static int (*read_snapshot)(void *,void *,unsigned);

static void release_copy(struct physical_slot *s)
{
    if(!s->resource) return;
    drop_snapshot(s->resource);
    s->resource=NULL;
    close(s->texture);
    close(s->ready);
}
static int wait_copy(int fd)
{
    struct pollfd p={.fd=fd,.events=POLLIN};
    int ret;
    do {ret=poll(&p,1,500);} while(ret<0 && errno==EINTR);
    return ret==1 && (p.revents&POLLIN);
}
static int mirror_locked(struct physical_slot *s)
{
    if(!s->resource || s->mirrored) return 1;
    struct dma_buf_sync sync={.flags=DMA_BUF_SYNC_START|DMA_BUF_SYNC_WRITE};
    if(!wait_copy(s->ready) || ioctl(s->plane,DMA_BUF_IOCTL_SYNC,&sync)) return 0;
    int ok=read_snapshot(s->resource,s->pixels,s->stride);
    sync.flags=DMA_BUF_SYNC_END|DMA_BUF_SYNC_WRITE;
    if(ioctl(s->plane,DMA_BUF_IOCTL_SYNC,&sync)) ok=0;
    if(ok) {
        s->mirrored=1;
    }
    return ok;
}
/* Called under slots_lock. Keep the only valid pixels until CPU backfill
 * succeeds. The next owner can adopt a pending slot with the same identity. */
static void retire_slot(struct physical_slot *s)
{
    release_copy(s);
    munmap(s->pixels, s->bytes);
    close(s->plane);
    memset(s, 0, sizeof(*s));
}

static int can_mirror_now(const struct physical_slot *s)
{
    if (!s->resource || s->mirrored) return 1;
    struct pollfd fence = {.fd = s->ready, .events = POLLIN};
    return poll(&fence, 1, 0) > 0 && (fence.revents & POLLIN);
}

static int retry_orphans(void)
{
    static unsigned cursor;
    int pending = 0, attempted = 0;
    pthread_mutex_lock(&slots_lock);
    unsigned start = cursor;
    for (unsigned n = 0; n < 16; ++n) {
        unsigned i = (start + n) % 16;
        struct physical_slot *s = &slots[i];
        if (!s->id || s->users) continue;
        if (!attempted && can_mirror_now(s)) {
            attempted = 1;
            cursor = (i + 1) % 16;
            if (mirror_locked(s)) {
                retire_slot(s);
                continue;
            }
        }
        pending = 1;
    }
    pthread_mutex_unlock(&slots_lock);
    return pending;
}

static int64_t retry_now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void physical_request(int sock)
{
    struct frame_frame p;
    int plane;
    if(!physical_peer(sock,"init.svc_debug_pid.vendor.hwcomposer-2-1") || !frame_packet(sock,&p,&plane,1,0)) return;
    struct stat st;
    int valid=!fstat(plane,&st) && (p.phase==PHYSICAL_GET || p.phase==PHYSICAL_CPU) && p.buffer_id;
    close(plane);
    if(!valid) return;
    pthread_mutex_lock(&slots_lock);
    struct physical_slot *s=NULL;
    for(unsigned i=0;i<16;i++) if(slots[i].id==p.buffer_id && slots[i].dev==st.st_dev &&
        slots[i].inode==st.st_ino) {s=&slots[i]; break;}
    if(s) s->direct=p.phase==PHYSICAL_GET;
    int output=-1;
    if(!s || !s->resource) p.phase=PHYSICAL_MIRROR;
    else if(p.phase==PHYSICAL_CPU) p.phase=mirror_locked(s) ? PHYSICAL_MIRROR : 0;
    else {
        /* Only the pending-fence case drops the lock. Pin the event fd; never
         * use the old slot contents after waiting unless identity still matches. */
        struct pollfd check={.fd=s->ready,.events=POLLIN};
        int ready=poll(&check,1,0)==1 && (check.revents&POLLIN);
        if (!ready) {
            uint32_t serial=s->generation;
            int fd=fcntl(s->ready,F_DUPFD_CLOEXEC,3);
            pthread_mutex_unlock(&slots_lock);
            ready=fd>=0 && wait_copy(fd);
            if(fd>=0) close(fd);
            pthread_mutex_lock(&slots_lock);
            if(s->id!=p.buffer_id || s->dev!=st.st_dev || s->inode!=st.st_ino ||
               s->generation!=serial || !s->resource) {ready=0;s=NULL;}
        }
        if(ready) {
            output=fcntl(s->texture,F_DUPFD_CLOEXEC,3);
            p.phase=output>=0 ? PHYSICAL_GPU : 0;
            p.width=s->width; p.height=s->height; p.stride=s->stride;
            p.serial=s->generation;
            p.sample=getpid();
        } else p.phase=0;
    }
    if (s && !s->users && s->mirrored) retire_slot(s);
    pthread_mutex_unlock(&slots_lock);
    frame_packet(sock,&p,&output,output>=0,1);
    if(output>=0) close(output);
}
static void *physical_server(void *arg)
{
    int listener=(int)(intptr_t)arg;
    struct pollfd fds[2] = {{listener, POLLIN, 0}, {retry_wake, POLLIN, 0}};
    int64_t due = retry_orphans() ? retry_now_ms() + 250 : 0;
    for(;;) {
        int64_t left = due - retry_now_ms();
        int timeout = !due ? -1 : left > 0 ? (int)left : 0;
        int rc = poll(fds, 2, timeout);
        if (rc < 0) { if (errno == EINTR) continue; break; }
        if (fds[1].revents & POLLIN) {
            uint64_t count;
            if (read(fds[1].fd, &count, sizeof(count)) > 0 && !due)
                due = retry_now_ms() + 250;
        }
        if (due && retry_now_ms() >= due)
            due = retry_orphans() ? retry_now_ms() + 250 : 0;
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        if (!(fds[0].revents & POLLIN)) continue;
        int sock=accept4(listener,NULL,NULL,SOCK_CLOEXEC);
        if(sock<0) {if(errno==EINTR) continue; break;}
        physical_timeout(sock);
        physical_request(sock);
        close(sock);
    }
    close(listener);
    pthread_mutex_lock(&slots_lock);
    close(retry_wake);
    retry_wake = -1;
    listening=0;
    pthread_mutex_unlock(&slots_lock);
    return NULL;
}
static int start_server(void)
{
    if(listening) return 1;
    struct sockaddr_un addr;
    socklen_t size=physical_address(&addr);
    int sd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);
    if(sd<0) return 0;
    pthread_t thread;
    if (bind(sd,(void*)&addr,size) || listen(sd,16)) { close(sd); return 0; }
    retry_wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (retry_wake < 0 || pthread_create(&thread,NULL,physical_server,(void*)(intptr_t)sd)) {
        if (retry_wake >= 0) close(retry_wake);
        retry_wake = -1;
        close(sd);
        return 0;
    }
    pthread_detach(thread);
    listening=1;
    return 1;
}
static uint64_t client_target_target_id(img_t *im,uint64_t usage)
{
    if(usage!=0x1b00 || im->format!=1 || im->w<1 || im->h<1 ||
       im->w>8192 || im->h>8192 || im->stride<im->w || im->stride>8192 || !frame_surfaceflinger()) return 0;
    const native_handle_t *h=im->handle ? im->handle : ahb_native_handle(im->buffer);
    if(!h || h->numFds<1) return 0;
    static void **mapper;
    static int (*get_id)(void *,const void *,uint64_t *),(*modifier)(void *,const void *,uint64_t *);
    static int (*fourcc)(void *,const void *,uint32_t *);
    if(!mapper) {
        void *ui=dlopen("libui.so",RTLD_NOW);
        if(ui) {
            mapper=dlsym(ui,"_ZN7android9SingletonINS_19GraphicBufferMapperEE9sInstanceE");
            get_id=dlsym(ui,"_ZN7android19GraphicBufferMapper11getBufferIdEPK13native_handlePm");
            modifier=dlsym(ui,"_ZN7android19GraphicBufferMapper22getPixelFormatModifierEPK13native_handlePm");
            fourcc=dlsym(ui,"_ZN7android19GraphicBufferMapper20getPixelFormatFourCCEPK13native_handlePj");
        }
    }
    uint64_t id=0,mod=~0ULL;
    uint32_t fmt=0;
    size_t bytes=(size_t)im->stride*im->h*4;
    struct stat st;
    if(!mapper || !*mapper || !get_id || !fourcc || !modifier ||
       get_id(*mapper,h,&id) || !id || fourcc(*mapper,h,&fmt) || fmt!=0x34324241u ||
       modifier(*mapper,h,&mod) || mod || fstat(h->data[0],&st) || st.st_size<(off_t)bytes) return 0;
    pthread_mutex_lock(&slots_lock);
    if(!export_snapshot) {
        export_target=glibc_ldr_proc("texture_cache_export_target");
        export_snapshot=glibc_ldr_proc("physical_snapshot_export");
        drop_snapshot=glibc_ldr_proc("physical_snapshot_drop");
        read_snapshot=glibc_ldr_proc("physical_snapshot_read");
    }
    struct physical_slot *s=NULL;
    if(export_snapshot && read_snapshot && drop_snapshot && start_server()) {
        for(unsigned i=0;i<16;i++) {
            if(slots[i].id==id && slots[i].dev==st.st_dev && slots[i].inode==st.st_ino) {s=&slots[i]; break;}
            if(!slots[i].id && !s) s=&slots[i];
        }
    }
    if(s && !s->id) {
        int fd=fcntl(h->data[0],F_DUPFD_CLOEXEC,3);
        void *map=fd<0 ? MAP_FAILED : mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
        if(map==MAP_FAILED) {if(fd>=0) close(fd); s=NULL;}
        else *s=(struct physical_slot){.id=id,.dev=st.st_dev,.inode=st.st_ino,.plane=fd,
            .width=im->w,.height=im->h,.stride=im->stride,.pixels=map,.bytes=bytes};
    }
    if(s) {im->slot=s; ++s->users; ++client_target_outputs;}
    pthread_mutex_unlock(&slots_lock);
    return s ? id : 0;
}
static void finish_slot(img_t *im)
{
    struct physical_slot *s=im->slot;
    if(!s) return;
    pthread_mutex_lock(&slots_lock);
    --client_target_outputs;
    if (!--s->users) {
        if (can_mirror_now(s) && mirror_locked(s)) retire_slot(s);
        else {
            uint64_t one = 1;
            ssize_t rc;
            do { rc = write(retry_wake, &one, sizeof(one)); } while (rc < 0 && errno == EINTR);
        }
    }
    im->slot = NULL;
    pthread_mutex_unlock(&slots_lock);
}
static int input_mirror(img_t *im)
{
    if(!im->slot) return 1;
    pthread_mutex_lock(&slots_lock);
    int ok=mirror_locked(im->slot);
    pthread_mutex_unlock(&slots_lock);
    return ok;
}
/* Called with the named FBO bound. A successful handoff owns both the GPU
 * copy and its recovery reader before the normal glReadPixels is skipped. */
static int physical_flush(img_t *im)
{
    struct physical_slot *s=im->slot;
    if(!s) return 0;
    int trace=texture_cache_flag("debug.waydroid.acc_trace",0);
    int64_t start=trace?texture_cache_ns():0;
    pthread_mutex_lock(&slots_lock);
    int64_t locked=trace?texture_cache_ns():0;
    if(!im->client_target_rendered && s->resource) {pthread_mutex_unlock(&slots_lock); return 1;}
    int fds[2];
    void *resource=NULL;
    int64_t timing[4]={0};
    int ok=im->client_target_rendered && s->direct;
    if(ok && export_target) {
        ok=!export_target(im->w,im->h,fds,&resource,texture_cache_flag("debug.waydroid.acc_pool",1),trace?timing:NULL);
        if(ok && trace) printf("texture-cache export lock=%lld create=%lld submit=%lld handle=%lld spare=%d",
            (long long)(locked-start),(long long)timing[0],(long long)timing[1],(long long)timing[2],(int)timing[3]);
    } else if(ok) ok=!export_snapshot(im->w,im->h,fds,&resource);
    im->client_target_rendered=0;
    release_copy(s);
    if(ok) {
        s->resource=resource; s->texture=fds[0]; s->ready=fds[1];
        s->generation=++generation; s->mirrored=0;
    }
    pthread_mutex_unlock(&slots_lock);
    return ok;
}
