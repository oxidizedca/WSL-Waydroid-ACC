#include <android/hardware_buffer.h>
#include <limits.h>

#define VIDEO_YV12 0x32315659

static pthread_once_t yv12_once = PTHREAD_ONCE_INIT;
static int (*lock_planes)(void *, uint64_t, int32_t, const ARect *, AHardwareBuffer_Planes *);
static int (*get_dataspace)(void *);
static int (*i420_to_argb)(const uint8_t *, int, const uint8_t *, int, const uint8_t *, int,
                           uint8_t *, int, const void *, int, int);
/* [standard][range]: BT.601, BT.709, BT.2020; limited and full. */
static const void *yvu_coefficients[3][2];
static pthread_key_t rgba_key;
static int have_rgba_key;

struct rgba_scratch {
    uint8_t *pixels;
    size_t capacity;
};

static void drop_rgba(void *ptr)
{
    struct rgba_scratch *scratch = ptr;
    free(scratch->pixels);
    free(scratch);
}

/* One conversion buffer per rendering thread, shared across BufferQueue slots. */
static uint8_t *rgba_pixels(size_t bytes)
{
    struct rgba_scratch *scratch = pthread_getspecific(rgba_key);
    if (!scratch) {
        scratch = calloc(1, sizeof(*scratch));
        if (!scratch || pthread_setspecific(rgba_key, scratch)) { free(scratch); return NULL; }
    }
    if (bytes > scratch->capacity) {
        uint8_t *pixels = realloc(scratch->pixels, bytes);
        if (!pixels) return NULL;
        scratch->pixels = pixels;
        scratch->capacity = bytes;
    }
    return scratch->pixels;
}

static void load_yv12(void)
{
    have_rgba_key = pthread_key_create(&rgba_key, drop_rgba) == 0;
    void *native = dlopen("libnativewindow.so", RTLD_NOW);
    void *yuv = dlopen("libyuv.so", RTLD_NOW);
    if (native) {
        lock_planes = dlsym(native, "AHardwareBuffer_lockPlanes");
        get_dataspace = dlsym(native, "_ZN7android28AHardwareBuffer_getDataSpaceEP15AHardwareBuffer");
    }
    if (!yuv) return;
    i420_to_argb = dlsym(yuv, "I420ToARGBMatrix");
    const char *names[3][2] = {
        {"kYvuI601Constants", "kYvuJPEGConstants"},
        {"kYvuH709Constants", "kYvuF709Constants"},
        {"kYvu2020Constants", "kYvuV2020Constants"}
    };
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 2; ++j)
            yvu_coefficients[i][j] = dlsym(yuv, names[i][j]);
}

static int init_yv12(img_t *im, void *ahb, uint64_t usage)
{
    pthread_once(&yv12_once, load_yv12);
    if (!ahb || !have_rgba_key || !lock_planes || !i420_to_argb || !g_ahb_acquire || !g_ahb_release ||
        !g_ahb_unlock || (usage & AHARDWAREBUFFER_USAGE_PROTECTED_CONTENT) ||
        im->w > INT_MAX / 4 || (size_t)im->h > SIZE_MAX / ((size_t)im->w * 4)) return 0;
    g_ahb_acquire(ahb);
    im->buffer = ahb;
    return 1;
}

static int yv12_pixels(img_t *im, uint8_t *rgba)
{
    int dataspace = get_dataspace ? get_dataspace(im->buffer) : 0;
    int standard = (dataspace >> 16) & 63, range = (dataspace >> 27) & 7;
    int matrix = standard == 1 ? 1 : standard == 6 ? 2 : 0;
    if (standard == 0 && im->h >= 720) matrix = 1;
    const void *coefficients = yvu_coefficients[matrix][range == 1];
    if (!coefficients) return 0;
    AHardwareBuffer_Planes planes = {0};
    if (lock_planes(im->buffer, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL, &planes))
        return 0;
    int ok = planes.planeCount == 3;
    for (int i = 0; ok && i < 3; ++i) {
        const AHardwareBuffer_Plane *p = &planes.planes[i];
        unsigned width = i ? ((unsigned)im->w + 1) / 2 : (unsigned)im->w;
        ok = p->data && p->pixelStride == 1 && p->rowStride >= width && p->rowStride <= INT_MAX;
    }
    if (ok) {
        /* Android supplies logical Y,U,V planes regardless of physical YV12 order.
         * libyuv writes BGRA bytes; exchange U/V and use its YVU matrix for RGBA. */
        const AHardwareBuffer_Plane *p = planes.planes;
        ok = i420_to_argb(p[0].data, p[0].rowStride, p[2].data, p[2].rowStride,
                          p[1].data, p[1].rowStride, rgba, im->w * 4, coefficients,
                          im->w, im->h) == 0;
    }
    return g_ahb_unlock(im->buffer, NULL) == 0 && ok;
}
