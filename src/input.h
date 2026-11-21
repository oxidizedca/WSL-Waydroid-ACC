#include "state.h"
static int layer_input_image(img_t *im,const void *handle)
{
    if(!input_shape(im->w,im->h) || im->format!=1 || im->stride<im->w ||
       im->stride>8192 || !frame_surfaceflinger() || !handle) return 0;
    const native_handle_t *h=handle;
    if(h->numFds<1) return 0;
    static void **mapper;
    static int (*id)(void*,const void*,uint64_t*),(*mod)(void*,const void*,uint64_t*),
               (*fmt)(void*,const void*,uint32_t*);
    if(!mapper) {
        void *ui=dlopen("libui.so",RTLD_NOW);
        if(ui) {
            mapper=dlsym(ui,"_ZN7android9SingletonINS_19GraphicBufferMapperEE9sInstanceE");
            id=dlsym(ui,"_ZN7android19GraphicBufferMapper11getBufferIdEPK13native_handlePm");
            mod=dlsym(ui,"_ZN7android19GraphicBufferMapper22getPixelFormatModifierEPK13native_handlePm");
            fmt=dlsym(ui,"_ZN7android19GraphicBufferMapper20getPixelFormatFourCCEPK13native_handlePj");
        }
    }
    uint64_t allocation,modifier; uint32_t fourcc;
    size_t size=(size_t)im->stride*im->h*4;
    struct stat st;
    if(!mapper || !*mapper || !id || !mod || !fmt || id(*mapper,h,&allocation) ||
       !allocation || mod(*mapper,h,&modifier) || modifier || fmt(*mapper,h,&fourcc) ||
       fourcc!=0x34324241u || fstat(h->data[0],&st) || st.st_size<(off_t)size) return 0;
    int fd=fcntl(h->data[0],F_DUPFD_CLOEXEC,3);
    if(fd<0)return 0;
    struct input_stamp mark=lookup_stamp(allocation,&st);
    im->layer_input_stat=st;
    im->layer_input_id=allocation;im->layer_input_plane=fd;im->layer_input_cached_fd=-1;
    /* RenderEngine can import a gralloc slot before its first queue. Keep
     * its real identity so a later bind sees the first committed generation. */
    if(mark.magic!=LAYER_INPUT_STAMP && mark.magic!=LAYER_INPUT_STAMP+1) return 0;
    void *map=mmap(NULL,size,PROT_READ,MAP_SHARED,fd,0);
    if(map==MAP_FAILED) return 0;
    im->vaddr=map;im->layer_input_owns_map=1;
    return 1;
}
static void (*texture_cache_forget)(void *);
static int layer_input_sample(img_t *im,unsigned target)
{
    struct input_stamp mark;
    struct frame_frame p;
    int fd=-1;
    mark=lookup_stamp(im->layer_input_id,&im->layer_input_stat);
    if(mark.magic!=LAYER_INPUT_STAMP && mark.magic!=LAYER_INPUT_STAMP+1) return 0;
    static int (*bind_input)(int,unsigned,int,int);
    static int (*bind_cached)(int,unsigned,int,int,void *,uint64_t,uint64_t,unsigned,int *);
    if(!bind_input) {
        bind_input=glibc_ldr_proc("shared_input_bind_input");
        bind_cached=glibc_ldr_proc("texture_cache_bind_input");
        texture_cache_forget=glibc_ldr_proc("texture_cache_forget_image");
    }
    int trace=texture_cache_flag("debug.waydroid.acc_trace",0);
    int64_t start=trace?texture_cache_ns():0;
    int cached=mark.magic==LAYER_INPUT_STAMP+1 && im->layer_input_cached_fd>=0 &&
               im->layer_input_cached_nonce==mark.nonce && im->layer_input_cached_serial==mark.serial;
    p.nonempty=cached;
    if(cached) fd=im->layer_input_cached_fd;
    if(bind_input && (cached || layer_input_request(LAYER_INPUT_GET,im->layer_input_id,im->layer_input_plane,mark,&p,&fd)==LAYER_INPUT_OK)) {
        int64_t got=trace?texture_cache_ns():0;
        int hit=0;
        /* A materialized plane still needs a successful GET (full digest)
         * on every bind. That GET authorizes the same immutable GPU snapshot
         * for this generation; only lazy generations may retain/skip the fd. */
        int ok=(cached || (p.width==(unsigned)im->w && p.height==(unsigned)im->h && p.stride==(unsigned)im->stride)) &&
               (bind_cached && texture_cache_forget && texture_cache_flag("debug.waydroid.acc_bind",1)
                ? bind_cached(fd,target,im->w,im->h,im,im->layer_input_id,mark.nonce,mark.serial,&hit)
                : bind_input(fd,target,im->w,im->h));
        if(trace && ok) printf("texture-cache bind cached=%d hit=%d get=%lld bind=%lld",
            cached,hit,(long long)(got-start),(long long)(texture_cache_ns()-got));
        if(!cached) {
            if(ok && p.nonempty) {
                if(im->layer_input_cached_fd>=0) close(im->layer_input_cached_fd);
                im->layer_input_cached_fd=fd;
                im->layer_input_cached_nonce=mark.nonce;im->layer_input_cached_serial=mark.serial;
            } else close(fd);
        }
        if(ok) {
            static uint64_t acked_nonce;
            if(!p.nonempty && acked_nonce!=mark.nonce) {
                struct frame_frame ack;
                if(layer_input_request(LAYER_INPUT_BOUND,im->layer_input_id,im->layer_input_plane,mark,&ack,NULL)==LAYER_INPUT_OK)
                    acked_nonce=mark.nonce;
            }
            return 1;
        }
    }
    if(mark.magic==LAYER_INPUT_STAMP+1 && layer_input_request(LAYER_INPUT_CPU,im->layer_input_id,im->layer_input_plane,mark,&p,NULL)!=LAYER_INPUT_OK) {
        /* The manager can be restarting with this unsampled generation in
         * its fd store. Wait only on the slow path for its committed mirror. */
        for(unsigned i=0;i<25;i++) {
            mark=lookup_stamp(im->layer_input_id,&im->layer_input_stat);
            if(mark.magic!=LAYER_INPUT_STAMP+1) break;
            usleep(20000);
        }
        if(mark.magic==LAYER_INPUT_STAMP+1) {printf("layer-input unavailable id=%llu",(unsigned long long)im->layer_input_id);return -1;}
    }
    return 0;
}
