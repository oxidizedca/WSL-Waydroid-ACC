#include "wire.h"
#include <sys/system_properties.h>
/* Allocation usage and permission to defer a CPU mirror are negotiated separately. */
static int layer_input_submit(shim_surf_t *e,int w,int h,int *confirmed_empty)
{
    static int (*set_usage)(ANativeWindow*,uint64_t);
    int role=ORDINARY;
    if(!e->win) return 0;
    for(int i=0;i<ORDINARY;i++) if(!strcmp(proc_name(),system_names[i])) {role=i;break;}
    if(role==ORDINARY && (getuid()<10000 || getuid()>19999)) return 0;
    if(!accepts_shape(role,w,h)) {
        if(e->layer_input_gpu && set_usage && !set_usage(e->win,0x33)) e->layer_input_gpu=0;
        e->layer_input_ready=0;
        return 0;
    }
    if(!e->layer_input_token) e->layer_input_token=arc4random_uniform(0x3fffffff)+1;
    pthread_once(&frame_once,frame_init);
    static int (*export_window)(int,int,int,int*);
    static int (*export_frame)(int,int,int*);
    if(!set_usage) {
        void *lib=dlopen("libnativewindow.so",RTLD_NOW);
        if(lib) set_usage=dlsym(lib,"ANativeWindow_setUsage");
    }
    if(!export_frame) export_frame=glibc_ldr_proc("shared_export_readbuffer");
    if(!set_usage || !export_frame || !frame_api.mapper || !*frame_api.mapper || !frame_api.dequeue ||
       !frame_api.queue || !frame_api.cancel || !frame_api.hardware_buffer || !frame_api.handle ||
       !frame_api.describe || !frame_api.id || !frame_api.modifier || !frame_api.fourcc) return 0;
    int sd=-1;
    char active[PROP_VALUE_MAX]={0};
    __system_property_get("waydroid.active_apps",active);
    int request_gpu=e->layer_input_ready!=0;
    if(!request_gpu && role==LAUNCHER && w==1920 && frame_full_height(h) && e->layer_input_visible &&
       !e->window_retry_retry_legacy) {
        request_gpu=1;
    }
    int gpu=e->layer_input_w==w && e->layer_input_h==h && request_gpu && !strcmp(active,"Waydroid");
    if(e->layer_input_gpu!=gpu) {
        if(set_usage(e->win,gpu?0x300:0x33)) {
            if(gpu && !e->layer_input_ready) e->window_retry_retry_legacy=1;
            return 0;
        }
        e->layer_input_gpu=gpu;
    }
    int rc=0,fd[3]={-1,-1,-1},acquire=-1;
    void *slot=NULL;
    struct frame_frame p={.magic=FRAME_MAGIC,.phase=LAYER_INPUT_PUBLISH,.serial=++e->frame_serial,.width=w,.height=h};
    if(frame_api.dequeue(e->win,&slot,&acquire)) {slot=NULL;goto out;}
    if(acquire>=0) {
        struct pollfd fence={.fd=acquire,.events=POLLIN};
        if(poll(&fence,1,500)!=1 || !(fence.revents&POLLIN)) goto out;
        close(acquire);acquire=-1;
    }
    AHardwareBuffer *buf=frame_api.hardware_buffer(slot);
    if(!buf) goto out;
    AHardwareBuffer_Desc d={0};
    frame_api.describe(buf,&d);
    const native_handle_t *handle=frame_api.handle(buf);
    uint64_t modifier; uint32_t fmt;
    if(!handle || handle->numFds<1 || d.width!=w || d.height!=h || d.layers!=1 ||
       d.format!=1 || !input_layout(w,h,d.stride) ||
       (d.usage!=0x933 && d.usage!=0x900 && d.usage!=0xb00) ||
       frame_api.id(*frame_api.mapper,handle,&p.buffer_id) || !p.buffer_id ||
       frame_api.modifier(*frame_api.mapper,handle,&modifier) || modifier ||
       frame_api.fourcc(*frame_api.mapper,handle,&fmt) || fmt!=0x34324241u) goto out;
    p.nonempty=gpu && d.usage==0xb00;
    p.alpha_fix=1;
    if(e->layer_input_visible || e->frame_verified) p.alpha_fix|=2;
    p.alpha_fix|=e->layer_input_token<<2;
    p.stride=d.stride;
    fd[2]=handle->data[0];
    int capability = role==ORDINARY ? APP_WINDOW_CAN_IMPORT : SYSTEM_WINDOW_CAN_CHECK;
    int use_fused = (e->layer_input_ready & capability) &&
                    p.nonempty && (p.alpha_fix & 2);
    if(use_fused && !export_window) export_window=glibc_ldr_proc("app_window_export_window");
    if(use_fused && export_window && !export_window(w,h,p.alpha_fix&1,fd))
        p.phase=role==ORDINARY ? APP_WINDOW_WINDOW : SYSTEM_WINDOW_SYSTEM_WINDOW;
    else if(export_frame(w,h,fd)) goto out;
    /* Dequeue and export can wait for SF to release another allocation.
     * Connect only when the complete publish packet can be sent immediately. */
    sd=layer_input_connect();
    if(sd<0) goto out;
    struct frame_frame reply;
    struct timeval blocking={0};
    if(setsockopt(sd,SOL_SOCKET,SO_RCVTIMEO,&blocking,sizeof(blocking)) ||
       !frame_packet(sd,&p,fd,3,1) || !frame_packet(sd,&reply,NULL,0,0) ||
       reply.buffer_id!=p.buffer_id || reply.serial!=p.serial) goto out;
    if(reply.phase!=LAYER_INPUT_OK) {
        *confirmed_empty = reply.phase==EMPTY_FRAME_EMPTY && p.phase==LAYER_INPUT_PUBLISH &&
                           role==LAUNCHER && w==1920 && frame_full_height(h) && !(p.alpha_fix&2);
        goto out;
    }
    e->layer_input_w=w;e->layer_input_h=h;e->layer_input_ready=reply.sample;
    if(!frame_api.queue(e->win,slot,-1)) {
        slot=NULL;rc=1;e->layer_input_visible=1;
        e->application_app_sampled = role == ORDINARY && reply.sample;
    }
out:
    if(slot) {
        if(frame_api.cancel(e->win,slot,acquire)) *confirmed_empty=0;
        acquire=-1;
    }
    if(acquire>=0)close(acquire);
    if(fd[0]>=0)close(fd[0]);
    if(fd[1]>=0)close(fd[1]);
    if(sd>=0) close(sd);
    if(!rc && e->layer_input_gpu) {if(!e->layer_input_ready) e->window_retry_retry_legacy=1;set_usage(e->win,0x33);e->layer_input_gpu=0;e->layer_input_ready=0;}
    return rc;
}
