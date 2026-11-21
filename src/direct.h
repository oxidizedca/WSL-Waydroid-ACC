#pragma once
#include "frame.h"
#include <android/hardware_buffer.h>
#include <cutils/native_handle.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/un.h>
#include <poll.h>
#include <android/log.h>

typedef void (*frame_read_fn)(int,int,int,int,unsigned,unsigned,void *);
static struct {
    int (*dequeue)(ANativeWindow*,void**,int*);
    int (*queue)(ANativeWindow*,void*,int);
    int (*cancel)(ANativeWindow*,void*,int);
    AHardwareBuffer *(*hardware_buffer)(void*);
    const native_handle_t *(*handle)(AHardwareBuffer*);
    void (*describe)(AHardwareBuffer*,AHardwareBuffer_Desc*);
    int (*lock)(AHardwareBuffer*,uint64_t,int,const ARect*,void**);
    int (*unlock)(AHardwareBuffer*,int*);
    void **mapper;
    int (*id)(void*,const native_handle_t*,uint64_t*);
    int (*fourcc)(void*,const native_handle_t*,uint32_t*);
    int (*modifier)(void*,const native_handle_t*,uint64_t*);
} frame_api;
static pthread_once_t frame_once=PTHREAD_ONCE_INIT;
static void frame_init(void)
{
    void *nw=dlopen("libnativewindow.so",RTLD_NOW), *ui=dlopen("libui.so",RTLD_NOW);
    if (!nw || !ui) return;
#define NW(field,name) frame_api.field=dlsym(nw,name)
    NW(dequeue,"ANativeWindow_dequeueBuffer"); NW(queue,"ANativeWindow_queueBuffer");
    NW(cancel,"ANativeWindow_cancelBuffer"); NW(hardware_buffer,"ANativeWindowBuffer_getHardwareBuffer");
    NW(handle,"AHardwareBuffer_getNativeHandle"); NW(describe,"AHardwareBuffer_describe");
    NW(lock,"AHardwareBuffer_lock"); NW(unlock,"AHardwareBuffer_unlock");
#undef NW
    frame_api.mapper=dlsym(ui,"_ZN7android9SingletonINS_19GraphicBufferMapperEE9sInstanceE");
    frame_api.id=dlsym(ui,"_ZN7android19GraphicBufferMapper11getBufferIdEPK13native_handlePm");
    frame_api.fourcc=dlsym(ui,"_ZN7android19GraphicBufferMapper20getPixelFormatFourCCEPK13native_handlePj");
    frame_api.modifier=dlsym(ui,"_ZN7android19GraphicBufferMapper22getPixelFormatModifierEPK13native_handlePm");
}

/* Confirm byte zero and row stride via the Android mapper before admitting a
 * linear minigbm allocation. Both words are restored while producer-owned. */
static int frame_layout(AHardwareBuffer *buf,int fd,const struct frame_frame *p)
{
    struct stat st;
    const size_t bytes=(size_t)p->stride*p->height*4;
    if(fstat(fd,&st) || st.st_size<(off_t)bytes) return 0;
    struct known {uint64_t id; ino_t ino; uint32_t stride;};
    static __thread struct known known[16];
    struct known *k=&known[p->buffer_id%16];
    if(k->id==p->buffer_id && k->ino==st.st_ino && k->stride==p->stride) return 1;
    volatile uint32_t *raw=mmap(NULL,bytes,PROT_READ,MAP_SHARED,fd,0);
    if(raw==MAP_FAILED) return 0;
    void *bits=NULL;
    int ok=0;
    if(!frame_api.lock(buf,0x33,-1,NULL,&bits)) {
        if(bits) {
            volatile uint32_t *a=bits;
            uint32_t x=a[0],y=a[p->stride];
            a[0]=~x; a[p->stride]=~y;
            ok=raw[0]==~x && raw[p->stride]==~y;
            a[0]=x; a[p->stride]=y;
            ok=ok && raw[0]==x && raw[p->stride]==y;
        }
        if(frame_api.unlock(buf,NULL)) ok=0;
    }
    munmap((void *)raw,bytes);
    if(ok) *k=(struct known){p->buffer_id,st.st_ino,p->stride};
    return ok;
}

static int frame_exchange(int sock,struct frame_frame *p,int *fds)
{
    struct frame_frame reply;
    if(!frame_packet(sock,p,fds,3,1) || !frame_packet(sock,&reply,NULL,0,0) ||
       reply.phase!=FRAME_READY || reply.buffer_id!=p->buffer_id || reply.serial!=p->serial) return 0;
    struct timeval blocking={0};
    if(setsockopt(sock,SOL_SOCKET,SO_RCVTIMEO,&blocking,sizeof(blocking))) return 0;
    p->phase=FRAME_WRITE;
    if(!frame_packet(sock,p,NULL,0,1) || !frame_packet(sock,&reply,NULL,0,0) ||
       reply.phase!=FRAME_DONE || reply.buffer_id!=p->buffer_id || reply.serial!=p->serial) return 0;
    p->sample=reply.sample;
    p->nonempty=reply.nonempty;
    return 1;
}

static int frame_compare(AHardwareBuffer *buf,const struct frame_frame *p,frame_read_fn read_pixels)
{
    size_t row=(size_t)p->width*4;
    uint32_t *ref=malloc(row*p->height);
    if(!ref) return 0;
    read_pixels(0,0,p->width,p->height,0x1908,0x1401,ref);
    void *bits=NULL;
    int equal=0;
    if(!frame_api.lock(buf,3,-1,NULL,&bits)) {
        equal=bits!=NULL;
        for(unsigned y=0;equal && y<p->height;y++) {
            uint32_t *r=ref+(size_t)(p->height-1-y)*p->width;
            if(p->alpha_fix) for(unsigned x=0;x<p->width;x++)
                if(r[x] && !(r[x]>>24)) r[x]|=0xff000000u;
            equal=!memcmp(r,(uint32_t*)bits+(size_t)y*p->stride,row);
        }
        if(frame_api.unlock(buf,NULL)) equal=0;
    }
    free(ref);
    return equal;
}

static int frame_eligible(const char *name,int w,int h)
{
    if(!strcmp(name,"com.android.settings")) return frame_full_height(h) && (w==698 || w==1222);
    if(!strcmp(name,"com.android.systemui") || !strcmp(name,"com.android.launcher3"))
        return w==1920 && (h==32 || h==132 || frame_full_height(h));
    return 0;
}

static int frame_submit(shim_surf_t *e,int w,int h,frame_read_fn read_pixels)
{
    if(!e->win || !frame_eligible(proc_name(),w,h)) return 0;
    pthread_once(&frame_once,frame_init);
    if(!frame_api.dequeue || !frame_api.queue || !frame_api.cancel || !frame_api.describe ||
       !frame_api.handle || !frame_api.hardware_buffer || !frame_api.mapper || !*frame_api.mapper ||
       !frame_api.lock || !frame_api.unlock || !frame_api.id || !frame_api.fourcc || !frame_api.modifier) return 0;
    static __thread int (*export_frame)(int,int,int*);
    if(!export_frame) export_frame=(void*)glibc_ldr_proc("shared_export_readbuffer");
    if(!export_frame) return 0;
    unsigned serial=++e->frame_serial;
    int sock=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
    if(sock<0) return 0;
    struct sockaddr_un addr={.sun_family=AF_UNIX,.sun_path=FRAME_SOCKET};
    struct timeval limit={.tv_usec=300000};
    struct ucred peer;
    socklen_t len=sizeof(peer);
    void *slot=NULL;
    int fds[3]={-1,-1,-1}, fence=-1, rc=0, audit=0;
    struct frame_frame p={.magic=FRAME_MAGIC,.width=w,.height=h,.serial=serial};
    if(connect(sock,(void*)&addr,sizeof(addr)) || getsockopt(sock,SOL_SOCKET,SO_PEERCRED,&peer,&len) ||
       peer.uid!=1000 || fcntl(sock,F_SETFL,0) || setsockopt(sock,SOL_SOCKET,SO_RCVTIMEO,&limit,sizeof(limit))) goto done;
    if(export_frame(w,h,fds)) goto done;
    if(frame_api.dequeue(e->win,&slot,&fence)) {slot=NULL;goto done;}
    if(fence>=0) {
        struct pollfd wait={fence,POLLIN,0};
        if(poll(&wait,1,200)!=1 || !(wait.revents&POLLIN)) goto done;
        close(fence); fence=-1;
    }
    AHardwareBuffer *buf=frame_api.hardware_buffer(slot);
    if(!buf) goto done;
    AHardwareBuffer_Desc d;
    frame_api.describe(buf,&d);
    if(d.width!=(unsigned)w || d.height!=(unsigned)h || d.layers!=1 || d.format!=1 ||
       (d.usage&0x4000) || (d.usage&0x33)!=0x33 || d.stride<d.width || d.stride>8192) goto done;
    const native_handle_t *handle=frame_api.handle(buf);
    uint32_t fourcc=0;
    uint64_t modifier=~(uint64_t)0;
    if(!handle || handle->numFds<1 || frame_api.id(*frame_api.mapper,handle,&p.buffer_id) ||
       !p.buffer_id || frame_api.fourcc(*frame_api.mapper,handle,&fourcc) || fourcc!=0x34324241u ||
       frame_api.modifier(*frame_api.mapper,handle,&modifier) || modifier!=0) goto done;
    p.stride=d.stride;
    p.alpha_fix=1;
    fds[2]=handle->data[0]; /* borrowed pixel-plane fd */
    if(!frame_layout(buf,fds[2],&p)) goto done;
    /* Announce every CPU write to Android's mapper, even after the layout is
     * known. The helper writes the same plane through its DMA-BUF mapping. */
    void *bits=NULL;
    if(frame_api.lock(buf,0x30,-1,NULL,&bits)) goto done;
    int filled=bits && frame_exchange(sock,&p,fds);
    int unlocked=frame_api.unlock(buf,NULL); /* complete release before queue/cancel */
    if(unlocked || !filled) goto done;
    /* SystemUI ends a splash by drawing transparent black. Falling back here
     * replays the opaque first frame saved before direct submission began.
     * Only trust an empty frame on an established full-screen SystemUI surface,
     * and compare the whole mapped slot to this very draw before queuing it. */
    if(!p.nonempty && !(e->frame_verified && w==1920 && frame_full_height(h) &&
                        !strcmp(proc_name(),"com.android.systemui"))) goto done;
    audit=!p.nonempty || e->frame_verified<3 || serial%120==0;
    if(audit && !frame_compare(buf,&p,read_pixels)) {e->frame_verified=0; goto done;}
    if(audit && e->frame_verified<3) ++e->frame_verified;
    if(frame_api.queue(e->win,slot,-1)==0) {slot=NULL;rc=1;}
done:
    if(slot) {frame_api.cancel(e->win,slot,fence);fence=-1;}
    if(fence>=0) close(fence);
    for(int i=0;i<2;i++) if(fds[i]>=0) close(fds[i]);
    close(sock);
    return rc;
}
