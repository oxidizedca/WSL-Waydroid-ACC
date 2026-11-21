/* x86_64 adapter for the Waydroid vendor HAL.
 * The device and buffer structures mirror the vendor HAL layout.
 * All three selected strategies must match before enabling our shm writer. */
#define _GNU_SOURCE
#include <android/native_window.h>
/* r17's minimal system/graphics.h omits the legacy aliases. */
#define HAL_TRANSFORM_FLIP_H ANATIVEWINDOW_TRANSFORM_MIRROR_HORIZONTAL
#define HAL_TRANSFORM_FLIP_V ANATIVEWINDOW_TRANSFORM_MIRROR_VERTICAL
#define HAL_TRANSFORM_ROT_90 ANATIVEWINDOW_TRANSFORM_ROTATE_90
#define HAL_TRANSFORM_ROT_180 ANATIVEWINDOW_TRANSFORM_ROTATE_180
#define HAL_TRANSFORM_ROT_270 ANATIVEWINDOW_TRANSFORM_ROTATE_270
#include <hardware/hwcomposer.h>
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <sys/system_properties.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include "frame.h"

struct wbuffer {
    void *wl_buffer;
    buffer_handle_t handle;
    uint32_t height, width, stride, format;
    bool is_shm;
    void *pixels;
    int size;
};
typedef void (*shm_writer)(void *, struct wbuffer *);
struct waydroid_device {
    hwc_composer_device_1_t base;
    const hwc_procs_t *procs;
    pthread_t vsync_thread, binder_thread, egl_worker_thread;
    int32_t period;
    void *display;
    void *metadata, *create_buffer;
    shm_writer update;
};
struct rect { int32_t left, top, right, bottom; };
static void **mapper;
static int (*mapper_lock)(void *, buffer_handle_t, unsigned, const struct rect *, void **);
static int (*mapper_unlock)(void *, buffer_handle_t, void *);
static shm_writer original;
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "frame-HWC", __VA_ARGS__)

static int (*mapper_id)(void *,buffer_handle_t,uint64_t *);
static int (*get_usage)(void *,buffer_handle_t,uint64_t *);
static __typeof__(((hwc_composer_device_1_t *)0)->prepare) vendor_prepare;

/* In full UI the home scene places a cropped, taller wallpaper below the
 * full-display launcher buffer. Keep this stack on the same Wayland surface
 * as the closing animation, including the first frame without a SKIP layer. */
static bool wallpaper_scene(const hwc_display_contents_1_t *d, size_t status)
{
    if (status < 2) return false;
    const hwc_layer_1_t *wall = &d->hwLayers[0], *home = &d->hwLayers[1];
    hwc_rect_t r = wall->displayFrame, app = home->displayFrame;
    if (r.left != 0 || r.top != 0 || r.right != 1920 || !frame_full_height(r.bottom) ||
        app.left != r.left || app.top != r.top || app.right != r.right ||
        app.bottom != r.bottom || wall->sourceCropf.bottom <= r.bottom ||
        wall->sourceCropf.top <= 0 || wall->transform || home->transform)
        return false;
    char active[PROP_VALUE_MAX] = {0};
    __system_property_get("waydroid.active_apps", active);
    return strcmp(active, "Waydroid") == 0;
}

/* The vendor groups the interval between the first and last SKIP layer into
 * one client target. Include the background as well as the translucent bars:
 * a transparent target above a separately switched wallpaper still flashes. */
#include "policy.h"
static int prepare(hwc_composer_device_1_t *dev,size_t count,hwc_display_contents_1_t **lists)
{
    hwc_display_contents_1_t *d=count && lists ? lists[HWC_DISPLAY_PRIMARY] : NULL;
    if(!d || !*mapper) return vendor_prepare(dev,count,lists);
    int result;
    if(group_physical(d,dev,count,lists,&result)) return result;
    size_t first=d->numHwLayers, status=d->numHwLayers, task=d->numHwLayers;
    for(size_t i=0;i<d->numHwLayers;i++) {
        hwc_layer_1_t *l=&d->hwLayers[i];
        if(l->compositionType==HWC_FRAMEBUFFER_TARGET) continue;
        if((l->flags&HWC_SKIP_LAYER) && first==d->numHwLayers) first=i;
        hwc_rect_t r=l->displayFrame;
        if(r.left==0 && r.right==1920) {
            if(r.top==0 && r.bottom==32) status=i;
            if(frame_full_height(r.bottom) && r.top==r.bottom-132) task=i;
        }
    }
    if(status>=task || task>=d->numHwLayers ||
       (first>=status && !wallpaper_scene(d,status)))
        return vendor_prepare(dev,count,lists);
    for(size_t i=0;i<=task;i++) {
        hwc_layer_1_t *l=&d->hwLayers[i];
        if(l->flags&HWC_SKIP_LAYER) continue;
        uint64_t usage=0;
        if((l->compositionType!=HWC_FRAMEBUFFER && l->compositionType!=HWC_OVERLAY) ||
           (l->flags&HWC_IS_CURSOR_LAYER) || !l->handle ||
           get_usage(*mapper,l->handle,&usage) || (usage&0x4000))
            return vendor_prepare(dev,count,lists);
    }
    hwc_layer_1_t *bottom=&d->hwLayers[0], *last=&d->hwLayers[task];
    uint32_t bottom_flags=bottom->flags, flags=last->flags;
    bottom->flags|=HWC_SKIP_LAYER;
    bottom->compositionType=HWC_FRAMEBUFFER;
    last->flags|=HWC_SKIP_LAYER;
    last->compositionType=HWC_FRAMEBUFFER;
    int rc=vendor_prepare(dev,count,lists);
    bottom->flags=bottom_flags;
    last->flags=flags;
    return rc;
}

#include "present.h"
#include "recovery.h"

static void update(void *display,struct wbuffer *b)
{
    if(client_target_attach_client(b) || layer_input_recover_device(b)) return;
    int target=(physical_handle && b->handle==physical_handle) || (frame_full_height(b->height) && (b->width==698 || b->width==1222 || b->width==1920)) ||
               (b->width==1920 && (b->height==32 || b->height==132));
    if (!target || !b->is_shm || b->format!=1 || !b->pixels || b->size!=(int)(b->width*b->height*4) ||
        b->stride<b->width || b->stride>8192 || !*mapper) {original(display,b);return;}
    const struct rect bounds={0,0,b->width,b->height};
    void *pixels=NULL;
    if(mapper_lock(*mapper,b->handle,GRALLOC_USAGE_SW_READ_OFTEN,&bounds,&pixels)) {
        original(display,b);return;
    }
    if(!pixels) {mapper_unlock(*mapper,b->handle,NULL);original(display,b);return;}
    if(weston_texture_present(display,b,pixels)) {
        mapper_unlock(*mapper,b->handle,NULL);
        return;
    }
    /* One view is shared by HWC, RenderEngine, screenshots and other Android readers. */
    for(unsigned y=0;y<b->height;y++) {
        const uint32_t *src=(const uint32_t*)pixels+(size_t)y*b->stride;
        uint32_t *dst=(uint32_t*)b->pixels+(size_t)y*b->width;
        for(unsigned x=0;x<b->width;x++) {
            uint32_t c=src[x]; dst[x]=(c&0xff00ff00u)|((c>>16)&255u)|((c&255u)<<16);
        }
    }
    mapper_unlock(*mapper,b->handle,NULL);
}

static int open_device(const hw_module_t *module, const char *name, hw_device_t **out)
{
    (void)module;
    void *lib = dlopen("/vendor/lib64/hw/hwcomposer.vendor-base.so", RTLD_NOW | RTLD_LOCAL);
    hw_module_t *base = lib ? dlsym(lib, HAL_MODULE_INFO_SYM_AS_STR) : NULL;
    if (!base || !base->methods || !base->methods->open) return -ENODEV;
    base->dso = lib;
    int rc = base->methods->open(base, name, out);
    if (rc || !*out) return rc ? rc : -ENODEV;
    struct waydroid_device *dev = (struct waydroid_device *)*out;
    void *(*select_meta)(int) = dlsym(lib,"_ZN15gralloc_handler31select_get_buffer_metadata_implE11GrallocType");
    void *(*select_create)(void *,int) = dlsym(lib,"_ZN15gralloc_handler25select_create_buffer_implEP7display11GrallocType");
    shm_writer (*select_update)(int) = dlsym(lib,"_ZN15gralloc_handler29select_update_shm_buffer_implE11GrallocType");
    void *ui = dlopen("libui.so", RTLD_NOW | RTLD_LOCAL);
    if (ui) {
        mapper = dlsym(ui,"_ZN7android9SingletonINS_19GraphicBufferMapperEE9sInstanceE");
        mapper_id = dlsym(ui,"_ZN7android19GraphicBufferMapper11getBufferIdEPK13native_handlePm");
        get_usage = dlsym(ui,"_ZN7android19GraphicBufferMapper8getUsageEPK13native_handlePm");
        mapper_lock = dlsym(ui,"_ZN7android19GraphicBufferMapper4lockEPK13native_handlejRKNS_4RectEPPv");
        mapper_unlock = dlsym(ui,"_ZN7android19GraphicBufferMapper6unlockEPK13native_handlePNS_4base14unique_fd_implINS4_13DefaultCloserEEE");
    }
    if (mapper && mapper_id && mapper_lock && mapper_unlock && select_meta && select_create && select_update &&
        dev->metadata == select_meta(3) && dev->update == select_update(3) &&
        dev->create_buffer == select_create(dev->display,3)) {
        weston_texture_install(lib,dev);
        original = dev->update;
        dev->update = update;
        if(get_usage && dev->base.prepare) {
            vendor_prepare=dev->base.prepare;
            dev->base.prepare=prepare;
        }
        LOG("initialized default gralloc shm writer uid=%d", getuid());
    } else LOG("using original gralloc writer; ABI or backend differs");
    return 0;
}
static hw_module_methods_t methods = {.open = open_device};
hwc_module_t HAL_MODULE_INFO_SYM = {.common = {
    .tag = HARDWARE_MODULE_TAG, .module_api_version = HWC_MODULE_API_VERSION_0_1,
    .hal_api_version = HARDWARE_HAL_API_VERSION, .id = HWC_HARDWARE_MODULE_ID,
    .name = "Waydroid shared-texture composer", .author = "WSL-Waydroid-ACC", .methods = &methods
}};
