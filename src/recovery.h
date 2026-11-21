#include "state.h"
/* A settings layer may become DEVICE between queue and composition. Ensure
 * the vendor's SHM writer sees this generation before it reads the plane. */
static bool layer_input_recover_device(struct wbuffer *b)
{
    if(!input_shape(b->width,b->height) || b->stride<b->width || b->format!=1 ||
       !b->handle || b->handle->numFds<1 || !mapper || !*mapper) return false;
    int fd=b->handle->data[0];
    uint64_t id=0;
    struct stat st;
    struct frame_frame p;
    if(mapper_id(*mapper,b->handle,&id) || fstat(fd,&st)) return false;
    struct input_stamp stamp=lookup_stamp(id,&st);
    if(stamp.magic!=LAYER_INPUT_STAMP+1 || layer_input_request(LAYER_INPUT_CPU,id,fd,stamp,&p,NULL)==LAYER_INPUT_OK) return false;
    if(manager && wl_proxy_get_version((struct wl_proxy*)manager)>=3) {
        wd_texture_manager_v1_hold_carrier(manager,b->wl_buffer);return true;
    }
    return false;
}
