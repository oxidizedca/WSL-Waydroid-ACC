/*
 * egl_image.c —— EGL_ANDROID_image_native_buffer 的 CPU 中转实现
 *
 * 第 53 轮最终形态：**EGLImage 句柄 = 真正的 Mesa 纹理 id**，纹理按 buffer 缓存。
 *
 * 背景（第 53 轮实证）：Skia/Ganesh 不调用 glEGLImageTargetTexture2DOES，
 * 它把 eglCreateImageKHR 的返回值直接当 GL 纹理名（fID = (GLuint)(uintptr_t)eglImage）。
 * 所以句柄必须是真纹理，否则 FBO 附件为空、draw 全落空（screencap 全透明 / HWC shm 全 0）。
 *
 * 三条链路：
 *   读：glBindTexture 命中我们的纹理 -> glTexSubImage2D 把 gralloc 内存灌进去（按需）
 *   写：glFramebufferTexture2D 命中 -> 标 dirty
 *        glFinish -> glReadPixels 把 dirty 纹理写回 gralloc
 *   缓存：纹理按 (EGLContext, native_handle/AHB, w,h,stride,fmt) 复用，
 *         避免每帧每层 32MB 纹理反复建/删（那是 boot loop 的主因）。
 *
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <android/log.h>
/* printf 在这个进程里输出到被重定向的 stdout，看不到；统一转 logcat */
#define printf(...) __android_log_print(ANDROID_LOG_INFO, "d3d12img", __VA_ARGS__)
#include <stdlib.h>
#include <pthread.h>
#include <sys/stat.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include <unistd.h>
#include <dlfcn.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <android/native_window.h>

#include "hardware/hardware.h"
#include "hardware/gralloc.h"

/* ---------- 与 egl_shim.c / loader 的连接 ---------- */
extern void *egl_shim_gl_proc(const char *name);   /* Mesa 的 dispatch stub */
extern void *glibc_ldr_proc(const char *name);     /* EGL 入口（懒初始化） */

#define EGLF(n) ((void *)glibc_ldr_proc(n))

/* ---------- GL 常量（避免依赖 GLES 头） ---------- */
#define GL_TEXTURE_2D_              0x0DE1
#define GL_RGBA_                    0x1908
#define GL_RGBA8_                   0x8058
#define GL_UNSIGNED_BYTE_           0x1401
#define GL_BGRA_EXT_                0x80E1
#define GL_FRAMEBUFFER_             0x8D40
#define GL_COLOR_ATTACHMENT0_       0x8CE0
#define GL_FRAMEBUFFER_BINDING_     0x8CA6
#define GL_TEXTURE_BINDING_2D_      0x8069
#define GL_UNPACK_ROW_LENGTH_       0x0CF2
#define GL_PACK_ALIGNMENT_          0x0D05
#define GL_PIXEL_UNPACK_BUFFER_BINDING_ 0x88EF
#define GL_TEXTURE_MIN_FILTER_      0x2801
#define GL_TEXTURE_MAG_FILTER_      0x2800
#define GL_TEXTURE_WRAP_S_          0x2802
#define GL_TEXTURE_WRAP_T_          0x2803
#define GL_NEAREST_                 0x2600
#define GL_CLAMP_TO_EDGE_           0x812F

#define EGL_NATIVE_BUFFER_ANDROID_  0x3140
#define EGL_NO_CONTEXT_             ((EGLContext)0)

#define IMG_MAGIC 0x47494d47u /* 'GMIG' */

/* 句柄小于这个值就按 GL 纹理 id 解释（纹理 id 是小整数，指针句柄在高位） */
#define IMG_HANDLE_MAX 0x100000u

/* gralloc usage 位（hardware/gralloc.h 里也有，这里只用两个） */
#define USAGE_HW_TEXTURE_ 0x100ull
#define USAGE_HW_RENDER_  0x200ull

/* ---------- ANativeWindowBuffer 布局（frameworks/native nativebase.h, LP64） ---------- */
typedef struct {
    int32_t magic, version;
    void *reserved0[4];
    void (*incRef)(void *);
    void (*decRef)(void *);
    int32_t width, height, stride, format, usage_deprecated;
    int32_t pad0;
    uintptr_t layerCount;
    void *reserved1[1];
    const void *handle;
    uint64_t usage;
    void *reserved_proc[7];
} anb_t;

typedef struct img_s {
    uint32_t magic;
    int wallpaper_fd_tag; /* 0 unchecked, -1 ineligible, otherwise owned fd + 1 */
    int bitmap_candidate;
    unsigned bitmap_texture;
    void *bitmap_context;
    uint64_t bitmap_serial;
    struct img_s *bitmap_next;
    void    *buffer;      /* 原始 ANativeWindowBuffer/AHardwareBuffer 指针 */
    void    *handle;      /* native_handle_t*（AHB 路径可能为 NULL） */
    int      w, h, stride, format;
    int      is_ahb;
    int      ahb_locked;
    unsigned char *vaddr; /* gralloc 映射到的 CPU 地址 */
    unsigned tex;
    void    *ctx;         /* 建这张纹理时的 EGLContext（纹理名是 per-context 的） */
    int      has_tex;
    int      needs_upload;
    int      dirty;
    int      owns_mapping;
    int      cache_idx;   /* 借自哪条缓存 */
    unsigned caller_tex;  /* 老语义：调用方自己建纹理，我们记住它以便回写 */
    int      has_caller_tex;
    struct stat layer_input_stat;
    uint64_t layer_input_id;
    int layer_input_plane,layer_input_cached_fd,layer_input_owns_map;
    uint64_t layer_input_cached_nonce;
    uint32_t layer_input_cached_serial;
    struct physical_slot *slot;
    uint64_t client_target_id;
    int client_target_rendered;
    long long client_target_last;
    unsigned char client_target_digest[32];
    int encoder, encoder_bound, encoder_written;
    void    *caller_ctx;
} img_t;

typedef struct {
    void    *key;         /* native_handle_t* 或 AHB 指针 */
    void    *ctx;         /* EGLContext（纹理名字是 per-context 的） */
    int      w, h, stride, format;
    unsigned tex;
    int      in_use;      /* 当前借用它的 image 数 */
    int      last_used;   /* LRU 计数 */
    /* 第 53 轮补充 D：CPU 映射和 buffer 身份【由条目持有】，
     * 因为 Skia 拿到纹理 id 后会立刻销毁 image —— 所有权放在 image 上就丢了。 */
    unsigned char *vaddr;
    void    *handle;
    void    *buffer;
    int      is_ahb;
    int      ahb_locked;
    int      dirty;
} texent_t;

typedef struct {
    uint32_t width, height, layers, format;
    uint64_t usage;
    uint32_t stride, rfu0;
    uint64_t rfu1;
} ahb_desc_t;

#define ANB_MAGIC_ 0x5f5f5041u

static const void *(*g_ahb_get_handle)(const void *);
static void (*g_ahb_describe)(const void *, void *);
static int  (*g_ahb_lock)(const void *, uint64_t, int32_t, const void *, void **);
static int  (*g_ahb_lock_info)(const void *, uint64_t, int32_t, const void *, void **, int32_t *, int32_t *);
static int  (*g_ahb_unlock)(const void *, int32_t *);
static void (*g_ahb_acquire)(void *);
static void (*g_ahb_release)(void *);
static void *(*g_anb_to_ahb)(void *);
static pthread_once_t ahb_once = PTHREAD_ONCE_INIT;

static void load_ahb_syms(void) {
    void *h = dlopen("libnativewindow.so", RTLD_NOW);
    if (h) {
        g_ahb_acquire = dlsym(h, "AHardwareBuffer_acquire");
        g_ahb_release = dlsym(h, "AHardwareBuffer_release");
        g_anb_to_ahb = dlsym(h, "ANativeWindowBuffer_getHardwareBuffer");
        g_ahb_get_handle = (const void *(*)(const void *))dlsym(h, "AHardwareBuffer_getNativeHandle");
        g_ahb_describe   = (void (*)(const void *, void *))dlsym(h, "AHardwareBuffer_describe");
        g_ahb_lock       = (int (*)(const void *, uint64_t, int32_t, const void *, void **))dlsym(h, "AHardwareBuffer_lock");
        g_ahb_lock_info  = (int (*)(const void *, uint64_t, int32_t, const void *, void **, int32_t *, int32_t *))dlsym(h, "AHardwareBuffer_lockAndGetInfo");
        g_ahb_unlock     = (int (*)(const void *, int32_t *))dlsym(h, "AHardwareBuffer_unlock");
    }
}

static void ahb_syms_init(void) {
    pthread_once(&ahb_once, load_ahb_syms);
}

static const void *ahb_native_handle(const void *buf) {
    ahb_syms_init();
    return g_ahb_get_handle ? g_ahb_get_handle(buf) : NULL;
}

#define MAX_IMG 64
/* Keep each lookup/use and detach/free atomic with respect to other EGL
 * threads. A GL call made during flush may reenter the shim on this thread. */
static pthread_mutex_t image_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static void unlock_images(pthread_mutex_t **lock) { pthread_mutex_unlock(*lock); }
#define LOCK_IMAGES() \
    pthread_mutex_t *guard __attribute__((cleanup(unlock_images))) = &image_lock; \
    pthread_mutex_lock(guard)

static img_t *g_imgs[MAX_IMG];
static _Atomic int encoder_bindings;
static int g_nimg;

#define MAX_TEXCACHE 24
static texent_t g_tc[MAX_TEXCACHE];
static int g_ntc;
static int g_tick;
static void tc_flush_entry(texent_t *e);
static int  tc_by_tex(unsigned tex);

/* ---------- 开关 ---------- */
/* 进程名（/proc/self/comm）是否出现在文件的空白分隔列表里 */
static int name_in_file(const char *path) {
    char me[64] = {0};
    FILE *c = fopen("/proc/self/comm", "r");
    if (!c) return 0;
    if (fgets(me, sizeof me, c)) { size_t m = strlen(me); while (m && (me[m-1]=='\n' || me[m-1]=='\r')) me[--m]=0; }
    fclose(c);
    if (!me[0]) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char tok[128];
    int hit = 0;
    while (fscanf(f, "%127s", tok) == 1) {
        if (!strcmp(tok, me)) { hit = 1; break; }
    }
    fclose(f);
    return hit;
}

/* The process allowlist selects texture-ID image handles. The denylist takes precedence. */
static int texid_on(void) {
    static int v = -1;
    if (v >= 0) return v;
    int on = 0;
    if (name_in_file("/data/local/tmp/texid_proc")) on = 1;
    if (on && name_in_file("/data/local/tmp/texid_not")) on = 0;
    v = on;
    return v;
}

/* 大小阈值：超过 /data/local/tmp/texid_max 里写的字节数就走旧路径（指针句柄）。
 * 用途：隔离"大纹理会不会把 AMD UMD 搞崩"。0/缺省 = 不限制。 */
static long texid_max_bytes(void) {
    static long v = -2;
    if (v != -2) return v;
    v = 0;
    FILE *f = fopen("/data/local/tmp/texid_max", "r");
    if (f) { long x = 0; if (fscanf(f, "%ld", &x) == 1) v = x; fclose(f); }
    return v;
}

/* ---------- gralloc ---------- */
static gralloc_module_t *g_gm;
static int g_gralloc_tried;
static int (*g_hw_get_module)(const char *, const struct hw_module_t **);

static void gralloc_probe(void) {
    if (g_gralloc_tried) return;
    g_gralloc_tried = 1;
    void *hl = dlopen("libhardware.so", RTLD_NOW);
    if (!hl) { printf("[img] libhardware.so not found"); return; }
    g_hw_get_module = (int (*)(const char *, const struct hw_module_t **))dlsym(hl, "hw_get_module");
    if (!g_hw_get_module) { printf("[img] hw_get_module missing"); return; }
    const struct hw_module_t *mod = NULL;
    int rc = g_hw_get_module("gralloc", &mod);
    if (rc != 0 || !mod) { printf("[img] hw_get_module(gralloc) rc=%d", rc); return; }
    g_gm = (gralloc_module_t *)mod;
    printf("[img] gralloc module: %s", mod->name ? mod->name : "?");
}

/* ---------- GL 入口 ---------- */
static void     (*gl_tex_image_2d)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
static void     (*gl_tex_sub_image_2d)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
static void     (*gl_tex_storage_2d)(unsigned, int, unsigned, int, int);
static void     (*gl_tex_parameteri)(unsigned, unsigned, int);
static void     (*gl_gen_textures)(int, unsigned *);
static void     (*gl_delete_textures)(int, const unsigned *);
static void     (*gl_bind_texture)(unsigned, unsigned);
static void     (*gl_bind_buffer)(unsigned, unsigned);
static void     (*gl_get_integerv)(unsigned, int *);
static void     (*gl_gen_framebuffers)(int, unsigned *);
static void     (*gl_delete_framebuffers)(int, const unsigned *);
static void     (*gl_bind_framebuffer)(unsigned, unsigned);
static void     (*gl_framebuffer_texture_2d)(unsigned, unsigned, unsigned, unsigned, int);
static void     (*gl_read_pixels)(int, int, int, int, unsigned, unsigned, void *);
static void     (*gl_pixel_storei)(unsigned, int);
static unsigned (*gl_get_error)(void);
static void *g_egl_current;


static void procs(void) {
    if (gl_tex_image_2d) return;
    gl_tex_image_2d          = (void (*)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *))egl_shim_gl_proc("glTexImage2D");
    gl_tex_sub_image_2d      = (void (*)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *))egl_shim_gl_proc("glTexSubImage2D");
    gl_tex_storage_2d        = (void (*)(unsigned, int, unsigned, int, int))egl_shim_gl_proc("glTexStorage2D");
    gl_tex_parameteri        = (void (*)(unsigned, unsigned, int))egl_shim_gl_proc("glTexParameteri");
    gl_gen_textures          = (void (*)(int, unsigned *))egl_shim_gl_proc("glGenTextures");
    gl_delete_textures       = (void (*)(int, const unsigned *))egl_shim_gl_proc("glDeleteTextures");
    gl_bind_texture          = (void (*)(unsigned, unsigned))egl_shim_gl_proc("glBindTexture");
    gl_bind_buffer           = (void (*)(unsigned, unsigned))egl_shim_gl_proc("glBindBuffer");
    gl_get_integerv          = (void (*)(unsigned, int *))egl_shim_gl_proc("glGetIntegerv");
    gl_gen_framebuffers      = (void (*)(int, unsigned *))egl_shim_gl_proc("glGenFramebuffers");
    gl_delete_framebuffers   = (void (*)(int, const unsigned *))egl_shim_gl_proc("glDeleteFramebuffers");
    gl_bind_framebuffer      = (void (*)(unsigned, unsigned))egl_shim_gl_proc("glBindFramebuffer");
    gl_framebuffer_texture_2d= (void (*)(unsigned, unsigned, unsigned, unsigned, int))egl_shim_gl_proc("glFramebufferTexture2D");
    gl_read_pixels           = (void (*)(int, int, int, int, unsigned, unsigned, void *))egl_shim_gl_proc("glReadPixels");
    gl_pixel_storei          = (void (*)(unsigned, int))egl_shim_gl_proc("glPixelStorei");
    gl_get_error             = (unsigned (*)(void))egl_shim_gl_proc("glGetError");
    g_egl_current            = EGLF("eglGetCurrentContext");
}

static void *cur_ctx(void) {
    if (!g_egl_current) g_egl_current = EGLF("eglGetCurrentContext");
    if (!g_egl_current) return (void *)1;   /* 没法判定，就当成同一个 context */
    typedef EGLContext (*F)(void);
    return (void *)((F)g_egl_current)();
}

static int ctx_current(void) { return cur_ctx() != NULL; }

/* ---------- 纹理缓存（按 buffer + context 复用） ---------- */

#include "mapping.h"
#include "bitmap.h"

static void tc_forget(int idx) {
    if (idx < 0 || idx >= g_ntc) return;
    --g_ntc;
    if (idx != g_ntc) {
        g_tc[idx] = g_tc[g_ntc];
        for (int i = 0; i < g_nimg; i++)
            if (g_imgs[i]->cache_idx == g_ntc) g_imgs[i]->cache_idx = idx;
    }
    memset(&g_tc[g_ntc], 0, sizeof g_tc[g_ntc]);
}

static int tc_find(void *key, void *ctx, int w, int h, int stride, int fmt) {
    for (int i = 0; i < g_ntc; i++) {
        texent_t *e = &g_tc[i];
        if (e->key == key && e->ctx == ctx && e->w == w && e->h == h &&
            e->stride == stride && e->format == fmt)
            return i;
    }
    return -1;
}

static void tc_evict_one(void) {
    int best = -1;
    for (int i = 0; i < g_ntc; i++) {
        texent_t *e = &g_tc[i];
        if (e->in_use) continue;
        if (best < 0 || e->last_used < g_tc[best].last_used) best = i;
    }
    if (best < 0) return;
    { texent_t *e = &g_tc[best];
      tc_flush_entry(e);
      /* in_use == 0: the entry owns precisely its own lock and reference.
       * Other EGLImages can have independent locks at the same address. */
      if (e->vaddr) drop_mapping(e->buffer, e->handle, e->is_ahb);
      e->vaddr = NULL;
    }
    if (gl_delete_textures && g_tc[best].tex) {
        unsigned t = g_tc[best].tex;
        gl_delete_textures(1, &t);
    }
    tc_forget(best);
}

/* 找或建一条缓存；返回纹理 id（0 = 失败） */
static unsigned tc_get(void *key, void *ctx, int w, int h, int stride, int fmt, int *out_idx) {
    int idx = tc_find(key, ctx, w, h, stride, fmt);
    if (idx >= 0) {
        g_tc[idx].last_used = ++g_tick;
        if (out_idx) *out_idx = idx;
        return g_tc[idx].tex;
    }
    if (g_ntc >= MAX_TEXCACHE) tc_evict_one();
    if (g_ntc >= MAX_TEXCACHE) return 0;
    if (!gl_gen_textures || !gl_bind_texture || !gl_tex_image_2d) return 0;

    int prev = 0;
    if (gl_get_integerv) gl_get_integerv(GL_TEXTURE_BINDING_2D_, &prev);
    unsigned t = 0;
    gl_gen_textures(1, &t);
    gl_bind_texture(GL_TEXTURE_2D_, t);
    unsigned glfmt = (fmt == 5) ? GL_BGRA_EXT_ : GL_RGBA_;
    /* 优先用不可变存储（Skia 自己也是这么建纹理的），失败再退回 TexImage2D */
    if (gl_tex_storage_2d) {
        gl_tex_storage_2d(GL_TEXTURE_2D_, 1, GL_RGBA8_, w, h);
        if (gl_get_error) {
            if (gl_get_error() != 0)
                gl_tex_image_2d(GL_TEXTURE_2D_, 0, GL_RGBA8_, w, h, 0, glfmt, GL_UNSIGNED_BYTE_, NULL);
        }
    } else {
        gl_tex_image_2d(GL_TEXTURE_2D_, 0, GL_RGBA8_, w, h, 0, glfmt, GL_UNSIGNED_BYTE_, NULL);
    }
    if (gl_tex_parameteri) {
        gl_tex_parameteri(GL_TEXTURE_2D_, GL_TEXTURE_MIN_FILTER_, GL_NEAREST_);
        gl_tex_parameteri(GL_TEXTURE_2D_, GL_TEXTURE_MAG_FILTER_, GL_NEAREST_);
        gl_tex_parameteri(GL_TEXTURE_2D_, GL_TEXTURE_WRAP_S_, GL_CLAMP_TO_EDGE_);
        gl_tex_parameteri(GL_TEXTURE_2D_, GL_TEXTURE_WRAP_T_, GL_CLAMP_TO_EDGE_);
        /* MAX_LEVEL=0：明确告诉驱动没有 mipmap 链 */
        gl_tex_parameteri(GL_TEXTURE_2D_, 0x813D, 0);
        gl_tex_parameteri(GL_TEXTURE_2D_, 0x813C, 0);
    }
    gl_bind_texture(GL_TEXTURE_2D_, (unsigned)prev);

    texent_t *e = &g_tc[g_ntc];
    memset(e, 0, sizeof *e);
    e->key = key; e->ctx = ctx;
    e->w = w; e->h = h; e->stride = stride; e->format = fmt;
    e->tex = t;
    e->last_used = ++g_tick;
    if (out_idx) *out_idx = g_ntc;
    g_ntc++;
    return t;
}

/* ---------- 句柄 <-> image ---------- */

static img_t *img_by_handle(void *h) {
    if (!h) return NULL;
    uintptr_t v = (uintptr_t)h;
    if (v < IMG_HANDLE_MAX) {
        for (int i = 0; i < g_nimg; i++)
            if (g_imgs[i]->has_tex && (uintptr_t)g_imgs[i]->tex == v) return g_imgs[i];
        return NULL;
    }
    for (int i = 0; i < g_nimg; i++)
        if ((void *)g_imgs[i] == h) return g_imgs[i];
    return NULL;
}

static void img_detach(img_t *im) {
    for (int i = 0; i < g_nimg; i++) {
        if (g_imgs[i] != im) continue;
        g_imgs[i] = g_imgs[--g_nimg];
        g_imgs[g_nimg] = NULL;
        return;
    }
}

/* ---------- 上传 / 回写 ---------- */

static int input_mirror(img_t *im);
static void img_upload_pending(img_t *im) {
    if (!im->has_tex || !im->vaddr || !input_mirror(im)) return;
    if (!ctx_current()) return;
    if (im->ctx && im->ctx != cur_ctx()) {
        return;
    }
    procs();
    if (!gl_tex_sub_image_2d || !gl_bind_texture) return;
    int prev = 0, prev_unpack = 0;
    if (gl_get_integerv) {
        gl_get_integerv(GL_TEXTURE_BINDING_2D_, &prev);
        gl_get_integerv(GL_PIXEL_UNPACK_BUFFER_BINDING_, &prev_unpack);
    }
    if (gl_bind_buffer && prev_unpack) gl_bind_buffer(0x88EF, 0);
    gl_bind_texture(GL_TEXTURE_2D_, im->tex);
    unsigned glfmt = (im->format == 5) ? GL_BGRA_EXT_ : GL_RGBA_;
    if (gl_pixel_storei && im->stride != im->w)
        gl_pixel_storei(GL_UNPACK_ROW_LENGTH_, im->stride);
    gl_tex_sub_image_2d(GL_TEXTURE_2D_, 0, 0, 0, im->w, im->h, glfmt, GL_UNSIGNED_BYTE_, im->vaddr);
    if (gl_pixel_storei && im->stride != im->w)
        gl_pixel_storei(GL_UNPACK_ROW_LENGTH_, 0);
    gl_bind_texture(GL_TEXTURE_2D_, (unsigned)prev);
    if (gl_bind_buffer && prev_unpack) gl_bind_buffer(0x88EF, (unsigned)prev_unpack);
    im->needs_upload = 0;
}

static void img_upload_to(img_t *im, unsigned target, unsigned tex) {
    if (!gl_tex_image_2d || !gl_get_integerv || !input_mirror(im)) return;
    unsigned glfmt = (im->format == 5) ? GL_BGRA_EXT_ : GL_RGBA_;
    if (gl_pixel_storei && im->stride != im->w)
        gl_pixel_storei(GL_UNPACK_ROW_LENGTH_, im->stride);
    gl_tex_image_2d(target, 0, GL_RGBA8_, im->w, im->h, 0, glfmt, GL_UNSIGNED_BYTE_, im->vaddr);
    if (gl_pixel_storei && im->stride != im->w)
        gl_pixel_storei(GL_UNPACK_ROW_LENGTH_, 0);
    (void)tex;
}

/* 回写：用临时 FBO（每次建/删，避免跨 context 复用 FBO 名字） */
#include "producer.h"
static void img_flush(img_t *im) {
    if(im->layer_input_owns_map) {im->dirty=0;return;}
    if (im->encoder_written) {im->dirty=0;return;}
    if (!im->vaddr || !im->dirty) return;
    if (!im->has_tex && !im->has_caller_tex) return;
    unsigned use_tex = im->has_tex ? im->tex : im->caller_tex;
    /* tex==0 说明我们没抓到"调用方绑的纹理"，这时候整面回写只会把 buffer 刷成 0，
     * 宁可不写（保留上一帧内容） */
    if (!use_tex) {
        im->dirty = 0;
        return;
    }
    if (!ctx_current()) return;
    if (im->ctx && im->ctx != cur_ctx()) {
        im->dirty = 0;
        return;
    }
    procs();
    if (!gl_gen_framebuffers || !gl_read_pixels) return;

    unsigned fbo = 0;
    gl_gen_framebuffers(1, &fbo);
    if (!fbo) return;
    int prev = 0;
    if (gl_get_integerv) gl_get_integerv(GL_FRAMEBUFFER_BINDING_, &prev);
    gl_bind_framebuffer(GL_FRAMEBUFFER_, fbo);
    gl_framebuffer_texture_2d(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D_, use_tex, 0);
    if (gl_pixel_storei) gl_pixel_storei(GL_PACK_ALIGNMENT_, 4);
    if(physical_flush(im)) {
        gl_bind_framebuffer(GL_FRAMEBUFFER_, (unsigned)prev);
        if(gl_delete_framebuffers) gl_delete_framebuffers(1,&fbo);
        im->dirty=0;
        return;
    }
    if (im->stride == im->w) {
        gl_read_pixels(0, 0, im->w, im->h, GL_RGBA_, GL_UNSIGNED_BYTE_, im->vaddr);
    } else {
        unsigned char *tmp = (unsigned char *)malloc((size_t)im->w * im->h * 4);
        if (tmp) {
            gl_read_pixels(0, 0, im->w, im->h, GL_RGBA_, GL_UNSIGNED_BYTE_, tmp);
            for (int y = 0; y < im->h; y++)
                memcpy(im->vaddr + (size_t)y * im->stride * 4, tmp + (size_t)y * im->w * 4,
                       (size_t)im->w * 4);
            free(tmp);
        }
    }
    /* [P0] 读回全 0 保护：源纹理没内容时，把 vaddr 整块写成 0 就是屏幕上的黑块。
     * SF 的全量回写会碰到一批“挂的是别人的纹理/还没渲染”的 img，读回必然全 0。
     * 这里采样一次，全 0 就放弃本次回写（保留 buffer 原内容）。 */
    {
        size_t npx = (size_t)im->w * (size_t)im->h;
        int nz = 0;
        for (size_t i = 0; i < npx; i += 97) {
            const unsigned char *q = (const unsigned char *)im->vaddr + i * 4;
            if (q[0] | q[1] | q[2] | q[3]) { nz = 1; break; }
        }
        if (!nz) {
            gl_bind_framebuffer(GL_FRAMEBUFFER_, prev ? (unsigned)prev : 0);
            if (gl_delete_framebuffers) gl_delete_framebuffers(1, &fbo);
            im->dirty = 0;
            return;
        }
    }
    gl_bind_framebuffer(GL_FRAMEBUFFER_, prev ? (unsigned)prev : 0);
    if (gl_delete_framebuffers) gl_delete_framebuffers(1, &fbo);
    im->dirty = 0;
}

/* 只回写给定矩形（EGL damage rects：x,y,w,h，左上原点，surface 坐标） */
static void img_flush_rect(img_t *im, int x, int y, int w, int h) {
    if (im->encoder_written) return;
    if (!im->vaddr || (!im->has_tex && !im->has_caller_tex)) return;
    if (!ctx_current()) return;
    procs();
    if (!gl_gen_framebuffers || !gl_read_pixels || !gl_pixel_storei) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > im->w) w = im->w - x;
    if (y + h > im->h) h = im->h - y;
    if (w <= 0 || h <= 0) return;

    unsigned tex = im->has_tex ? im->tex : im->caller_tex;
    unsigned fbo = 0;
    gl_gen_framebuffers(1, &fbo);
    if (!fbo) return;
    int prev = 0;
    if (gl_get_integerv) gl_get_integerv(GL_FRAMEBUFFER_BINDING_, &prev);
    gl_bind_framebuffer(GL_FRAMEBUFFER_, fbo);
    gl_framebuffer_texture_2d(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D_, tex, 0);
    gl_pixel_storei(GL_PACK_ALIGNMENT_, 4);
    unsigned char *tmp = (unsigned char *)malloc((size_t)w * h * 4);
    if (tmp) {
        int gl_y = im->h - (y + h);   /* GL 原点在左下，EGL damage 在左上 */
        gl_read_pixels(x, gl_y, w, h, GL_RGBA_, GL_UNSIGNED_BYTE_, tmp);
        for (int row = 0; row < h; row++) {
            unsigned char *dst = im->vaddr + (size_t)(y + (h - 1 - row)) * im->stride * 4 + (size_t)x * 4;
            memcpy(dst, tmp + (size_t)row * w * 4, (size_t)w * 4);
        }
        free(tmp);
    }
    gl_bind_framebuffer(GL_FRAMEBUFFER_, prev ? (unsigned)prev : 0);
    if (gl_delete_framebuffers) gl_delete_framebuffers(1, &fbo);
}

/* ---------- 按纹理 id 查缓存条目（image 死了也认得） ---------- */
static int tc_by_tex(unsigned tex) {
    if (!tex || tex >= IMG_HANDLE_MAX) return -1;
    for (int i = 0; i < g_ntc; i++)
        if (g_tc[i].tex == tex) return i;
    return -1;
}

/* 用条目自己的 vaddr/几何回写（借 img_flush 的实现，构造一个临时 img_t） */
static void tc_flush_entry(texent_t *e) {
    if (!e || !e->dirty || !e->tex || !e->vaddr) return;
    for(int i=0;i<g_nimg;i++) {
        img_t *im=g_imgs[i];
        if(im->slot && im->vaddr==e->vaddr && im->tex==e->tex) {
            im->dirty=1;
            img_flush(im);
            if(!im->dirty) e->dirty=0;
            return;
        }
    }
    img_t tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.magic = IMG_MAGIC;
    tmp.w = e->w; tmp.h = e->h; tmp.stride = e->stride; tmp.format = e->format;
    tmp.vaddr = e->vaddr; tmp.tex = e->tex; tmp.has_tex = 1; tmp.dirty = 1;
    img_flush(&tmp);
    if (!tmp.dirty) e->dirty = 0;
}

static int frame_surfaceflinger(void)
{
    static __thread int checked_pid, sf;
    if(checked_pid!=getpid()) {
        char name[96]={0};
        int fd=open("/proc/self/cmdline",O_RDONLY);
        if(fd>=0) {ssize_t n=read(fd,name,sizeof(name)-1);close(fd);sf=n>0 && strstr(name,"surfaceflinger");}
        checked_pid=getpid();
    }
    return sf;
}

/* ---------- 对外入口 ---------- */

#include "input.h"
#include "yv12.h"
#include "wallpaper.h"
void *egl_img_create(unsigned int target, void *buffer, const void *attribs) {
    LOCK_IMAGES();
    (void)attribs;
    if (target != EGL_NATIVE_BUFFER_ANDROID_ || !buffer) return NULL;

    if (g_nimg >= MAX_IMG) {
        printf("[img] table full (%d), fallback to Mesa", g_nimg);
        return NULL;
    }

    gralloc_probe();

    int lw = 0, lh = 0, lstride = 0, lfmt = 0;
    uint64_t lusage = 0;
    void *lhandle = NULL;
    void *ahb_real = NULL;
    uint32_t first = *(volatile uint32_t *)buffer;
    int is_anb = (first == ANB_MAGIC_);
    if (is_anb) {
        anb_t *anb = (anb_t *)buffer;
        lw = anb->width; lh = anb->height; lstride = anb->stride; lfmt = anb->format;
        lusage = anb->usage;
        lhandle = (void *)anb->handle;
    } else {
        ahb_syms_init();
        ahb_real = first == 0x5f626672u ? (g_anb_to_ahb ? g_anb_to_ahb(buffer) : NULL) : buffer;
        if (!ahb_real || !g_ahb_describe) return NULL;
        ahb_desc_t desc = {0};
        g_ahb_describe(ahb_real, &desc);
        lw = desc.width; lh = desc.height; lstride = desc.stride;
        lfmt = desc.format; lusage = desc.usage;
    }

    img_t *im = (img_t *)calloc(1, sizeof(*im));
    if (!im) return NULL;
    im->magic = IMG_MAGIC;
    im->bitmap_candidate = lfmt == 1 && lusage == USAGE_HW_TEXTURE_;
    im->buffer = is_anb ? buffer : ahb_real;
    im->w = lw; im->h = lh;
    im->stride = lstride; im->format = lfmt;
    im->handle = lhandle;
    im->is_ahb = !is_anb;
    im->cache_idx = -1;
    if (im->w <= 0 || im->h <= 0 || im->stride <= 0) {
        printf("[img] bad geometry %dx%d stride=%d handle=%p", im->w, im->h, im->stride, im->handle);
        free(im);
        return NULL;
    }
    if (lfmt == VIDEO_YV12) {
        if (!init_yv12(im, ahb_real, lusage)) { free(im); return NULL; }
        g_imgs[g_nimg++] = im;
        return im;
    }
    if((lusage==0x933 || lusage==0x900 || lusage==0xb00) && frame_surfaceflinger() && input_shape(im->w,im->h) && layer_input_image(im,lhandle ? lhandle : ahb_native_handle(ahb_real))) {
        g_imgs[g_nimg++]=im;
        return im;
    }
    if (!lock_mapping(im)) {
        if(im->layer_input_id) close(im->layer_input_plane);
        printf("[img] no CPU mapping (handle=%p is_ahb=%d), give up", im->handle, is_anb);
        free(im);
        return NULL;
    }

    long lim = texid_max_bytes();
    if(im->ahb_locked && g_ahb_describe) {
        ahb_desc_t desc={0};
        g_ahb_describe(im->buffer,&desc);
        lusage=desc.usage;
    }
    im->client_target_id=client_target_target_id(im,lusage);
    im->encoder=im->format==1 && (lusage & 0x10200)==0x10200 && frame_surfaceflinger();
    if (!texid_on() || (lim > 0 && (long)im->w * im->h * 4 > lim)) {
        /* 旧路径：返回指针句柄（r52 行为） */
        g_imgs[g_nimg++] = im;
        return im;
    }

    /* 新路径：句柄 = 真纹理 id，纹理按 buffer 缓存 */
    procs();
    void *ctx = cur_ctx();
    void *key = im->handle ? im->handle : (void *)(uintptr_t)0x1000u;   /* AHB 用常量键 + 指针 */
    if (!im->handle) key = im->buffer;
    int idx = -1;
    unsigned t = tc_get(key, ctx, im->w, im->h, im->stride, im->format, &idx);
    if (!t) {
        printf("[img] no texture, fallback to pointer handle");
        g_imgs[g_nimg++] = im;
        return im;
    }
    g_tc[idx].in_use++;
    im->ctx = ctx;
    im->tex = t;
    im->has_tex = 1;
    im->cache_idx = idx;

    /* 第 53 轮补充 D：把 CPU 映射的所有权交给缓存条目。
     * Skia 拿到纹理 id 后立刻 eglDestroyImageKHR，image 没了但纹理还在被画，
     * 所以"纹理 -> gralloc 内存"这条绑定必须活得比 image 长。 */
    {
        texent_t *e = &g_tc[idx];
        if (!e->vaddr) {
            im->owns_mapping = 0;
            e->vaddr = im->vaddr;
            e->handle = im->handle;
            e->buffer = im->buffer;
            e->is_ahb = im->is_ahb;
            e->ahb_locked = im->ahb_locked;
            im->ahb_locked = 0;      /* 之后由条目负责解锁 */
        } else {
            /* 条目已经锁好了，把这次多锁的一份还掉 */
            drop_mapping(im->buffer, im->handle, im->is_ahb);
            im->owns_mapping = im->ahb_locked = 0;
            im->vaddr = e->vaddr;
        }
        (void)lusage;
    }

    /* 渲染目标也要"先上传上一帧内容"，这样应用只重绘脏区域时不会把其余部分抹成 0。 */
    im->needs_upload = 1;
    im->dirty = 0;

    g_imgs[g_nimg++] = im;
    /* 立刻上传：此刻纹理不可能挂在 FBO 上（放 glBindTexture 里会踩 feedback loop） */
    img_upload_pending(im);
    return (void *)(uintptr_t)t;
}

static void rt_remove(img_t *im);   /* 定义在 RT 集合那一节，先声明 */

int egl_img_destroy(void *img) {
    LOCK_IMAGES();
    img_t *im = img_by_handle(img);
    if (!im || im->magic != IMG_MAGIC) return 0;
    if (im->format == VIDEO_YV12) {
        img_detach(im);
        g_ahb_release(im->buffer);
        free(im);
        return 1;
    }
    if (wallpaper.owner==im) forget_wallpaper();
    if (im->wallpaper_fd_tag>0) close(im->wallpaper_fd_tag-1);
    forget_bitmap(im);
    if(im->layer_input_id) {
        if(texture_cache_forget) texture_cache_forget(im);
        if(im->layer_input_cached_fd>=0) close(im->layer_input_cached_fd);
        close(im->layer_input_plane);
        if(im->layer_input_owns_map) {
            img_detach(im); rt_remove(im);
            munmap(im->vaddr,(size_t)im->stride*im->h*4);
            free(im);return 1;
        }
    }
    if (im->cache_idx >= 0 && im->cache_idx < g_ntc && g_tc[im->cache_idx].tex == im->tex) {
        texent_t *e = &g_tc[im->cache_idx];
        if (e->dirty) tc_flush_entry(e);
        if (e->in_use > 0) e->in_use--;
        e->last_used = ++g_tick;
        /* 注意：不解锁、不删纹理。映射和纹理都归条目所有（image 会被 Skia 早早销毁）。 */
    }
    /* [r76 越界修复] free 之前先摘 RT 表并把 magic 清掉：
     * 双保险 —— 任何残留指针都过不了后续的存活/magic 校验，不会再被 glFinish 回写踩到。 */
    img_detach(im);
    if(im->encoder_bound) --encoder_bindings;
    rt_remove(im);
    im->magic = 0;
    finish_slot(im);
    if (im->owns_mapping) drop_mapping(im->buffer, im->handle, im->is_ahb);
    free(im);
    return 1;
}

int egl_img_is_ours(void *img) {
    LOCK_IMAGES();
    return img_by_handle(img) != NULL;
}


void egl_img_flush_dirty(void) {
    LOCK_IMAGES();
    for (int i = 0; i < g_ntc; i++) tc_flush_entry(&g_tc[i]);
    for (int i = 0; i < g_nimg; i++)
        if (g_imgs[i]->dirty) img_flush(g_imgs[i]);
}

void egl_img_flush_all(void) { egl_img_flush_dirty(); }

/* [P0] 渲染目标集合。SF 的 RenderEngine 只在创建时把 texture attach 到 FBO 一次，
 * 之后每帧只 glBindFramebuffer 复用 —— dirty 在第一次 flush 后就被清掉，
 * 所以「按 dirty 回写」看不到动画。这里把 note_fbo 命中过的 img 永久登记下来，
 * 每帧按这张表强制回写；没有命中过的 img（OES 采样输入、app 的 layer buffer）
 * 一律不碰，避免黑块 / 帧残留 / 壁纸黑。 */
static img_t *g_rt_out[64];
static int g_rt_n;
static void rt_add(img_t *im) {
    if (!im || g_rt_n >= 64) return;
    for (int i = 0; i < g_rt_n; i++) if (g_rt_out[i] == im) return;
    g_rt_out[g_rt_n++] = im;
}
/* [r76 越界修复] rt 表原本只有"加"没有"删"：img 被 egl_img_destroy 释放后仍留在表里，
 * 每帧 glFinish 的强制回写都会解引用已释放的指针（UAF 读）；如果那块内存被 malloc
 * 复用成新的 img_t，magic 校验照样通过，就会拿错的 vaddr/几何去回写。 */
static void rt_remove(img_t *im) {
    for (int i = 0; i < g_rt_n; i++) {
        if (g_rt_out[i] != im) continue;
        g_rt_out[i] = g_rt_out[--g_rt_n];
        g_rt_out[g_rt_n] = NULL;
        return;
    }
}
/* 只做指针比较，绝不 deref：用来判断 rt 表里的 img 是否已经不在 g_imgs[] 里了 */
static int img_alive(img_t *im) {
    for (int i = 0; i < g_nimg; i++) if (g_imgs[i] == im) return 1;
    return 0;
}
void egl_img_flush_render_targets(void) {
    LOCK_IMAGES();
    for (int i = 0; i < g_rt_n; i++) {
        img_t *im = g_rt_out[i];
        if (!im) continue;
        /* [r76 越界修复] 先确认 img 还活在 g_imgs[] 里（纯指针比较），再碰它的字段。 */
        if (!img_alive(im)) continue;
        if (im->magic != IMG_MAGIC || !im->vaddr) continue;
        im->dirty = 1;
        img_flush(im);
    }
}

int egl_img_note_fbo(unsigned int tex) {
    LOCK_IMAGES();
    if (!tex || (uintptr_t)tex >= IMG_HANDLE_MAX) return 0;
    /* A0) 先按纹理 id 找缓存条目 —— image 可能已经被 Skia 销毁了，
     * 但纹理和它对应的 gralloc 内存还在我们手上（补充 D 的所有权改动）。 */
    {
        int ti = tc_by_tex((unsigned)tex);
        if (ti >= 0) {
            g_tc[ti].dirty = 1;
            img_t *im0 = img_by_handle((void *)(uintptr_t)tex);
            if (im0) { im0->dirty = 1; rt_add(im0); }
            return 1;
        }
    }
    /* A) 挂的是我们自己的纹理 */
    img_t *im = img_by_handle((void *)(uintptr_t)tex);
    if (im) {
        if (im->ctx && im->ctx != cur_ctx()) {
            return 0;
        }
        if (im->has_tex) {
            im->dirty = 1;
            im->needs_upload = 0;
        }
        rt_add(im);
        return 1;
    }
    /* B) 挂的是 HWUI 自己建、我们记过账的纹理 */
    for (int i = 0; i < g_nimg; i++) {
        if (g_imgs[i]->has_caller_tex && g_imgs[i]->caller_tex == tex) {
            g_imgs[i]->dirty = 1;
            rt_add(g_imgs[i]);
            return 1;
        }
    }
    return 1;
}

/* An external sampler needs a real EGL image. GLES disallows glTexImage2D
 * with GL_TEXTURE_EXTERNAL_OES. Upload into a 2D texture, then attach its
 * storage to the external target in the same glibc EGL display/context. */
static int attach_pixels(img_t *im, unsigned target, const void *pixels, int stride, unsigned format)
{
    static EGLDisplay (*display)(void);
    static EGLImage (*create)(EGLDisplay,EGLContext,EGLenum,EGLClientBuffer,const EGLAttrib*);
    static EGLBoolean (*destroy)(EGLDisplay,EGLImage);
    static void (*attach)(unsigned,void*);
    static const char *(*get_string)(unsigned);
    if(!display) {
        get_string = egl_shim_gl_proc("glGetString");
        display=EGLF("eglGetCurrentDisplay"); create=EGLF("eglCreateImage");
        destroy=EGLF("eglDestroyImage"); attach=egl_shim_gl_proc("glEGLImageTargetTexture2DOES");
    }
    if(!display || !create || !destroy || !attach || !gl_gen_textures || !gl_delete_textures ||
       !gl_bind_texture || !gl_tex_image_2d || !gl_tex_parameteri || !gl_get_integerv ||
       !gl_bind_buffer || !gl_pixel_storei) return 0;
    EGLDisplay d=display();
    EGLContext ctx=cur_ctx();
    if(!d || !ctx) return 0;
    const void *original=pixels;
    enum wallpaper_action action=WALL_PASSTHROUGH;
    if (target==0x8d65 && pixels==im->vaddr && im->format==1 && gl_get_error) {
        action=wallpaper_pixels(im,d,ctx,pixels,destroy);
        if (action==WALL_REBIND) {
            attach(target,wallpaper.image);
            if (!gl_get_error()) return 1;
            forget_wallpaper();
            action=WALL_PASSTHROUGH;
        }
        if (action==WALL_UPLOAD) pixels=wallpaper.pixels;
    }
upload_pixels: ;
    const char *version = get_string ? get_string(0x1f02) : NULL;
    if (!version) return 0;
    int es2 = strncmp(version, "OpenGL ES 2.", 12) == 0;
    const char *ext = es2 ? get_string(0x1f03) : NULL;
    int row_state = !es2 || (ext && strstr(ext, "GL_EXT_unpack_subimage"));
    int pbo_state = !es2 || (ext && strstr(ext, "GL_NV_pixel_buffer_object"));
    if (!row_state && stride != im->w) return 0;
    unsigned tex=0; int prev=0,unpack=0,saved[4];
    const unsigned names[4]={0x0cf5,0x0cf2,0x0cf3,0x0cf4};
    int states = row_state ? 4 : 1;
    gl_get_integerv(GL_TEXTURE_BINDING_2D_,&prev);
    if (pbo_state) gl_get_integerv(GL_PIXEL_UNPACK_BUFFER_BINDING_,&unpack);
    for(int i=0;i<states;i++) gl_get_integerv(names[i],&saved[i]);
    if(unpack) gl_bind_buffer(0x88ec,0);
    gl_gen_textures(1,&tex);
    if (!tex) {
        if (unpack) gl_bind_buffer(0x88ec,unpack);
        return 0;
    }
    gl_bind_texture(GL_TEXTURE_2D_,tex);
    gl_tex_parameteri(GL_TEXTURE_2D_,GL_TEXTURE_MIN_FILTER_,GL_NEAREST_);
    gl_tex_parameteri(GL_TEXTURE_2D_,GL_TEXTURE_WRAP_S_,GL_CLAMP_TO_EDGE_);
    gl_tex_parameteri(GL_TEXTURE_2D_,GL_TEXTURE_WRAP_T_,GL_CLAMP_TO_EDGE_);
    if (!es2) gl_tex_parameteri(GL_TEXTURE_2D_,0x813d,0);
    gl_pixel_storei(names[0],4);
    if (row_state) {
        gl_pixel_storei(names[1],stride);
        gl_pixel_storei(names[2],0); gl_pixel_storei(names[3],0);
    }
    gl_tex_image_2d(GL_TEXTURE_2D_,0,es2 ? format : GL_RGBA8_,im->w,im->h,0,
                    format,GL_UNSIGNED_BYTE_,pixels);
    const EGLAttrib attrs[]={0x30d2,1,EGL_NONE}; /* EGL_IMAGE_PRESERVED_KHR */
    EGLImage image=create(d,ctx,0x30b1,(EGLClientBuffer)(uintptr_t)tex,attrs);
    gl_bind_texture(GL_TEXTURE_2D_,prev);
    for(int i=0;i<states;i++) gl_pixel_storei(names[i],saved[i]);
    if(unpack) gl_bind_buffer(0x88ec,unpack);
    int ok=image!=NULL;
    if (image) {
        attach(target,image);
        if (action==WALL_UPLOAD) {
            ok=gl_get_error()==0;
            if (ok) wallpaper.image=image;
        }
        if (image!=wallpaper.image) destroy(d,image);
    }
    gl_delete_textures(1,&tex);
    if (!ok && action==WALL_UPLOAD) {
        forget_wallpaper();
        action=WALL_PASSTHROUGH;
        pixels=original;
        goto upload_pixels;
    }
    return ok;
}

int egl_img_target_texture(unsigned int target, void *img) {
    LOCK_IMAGES();
    img_t *im = img_by_handle(img);
    if (!im || im->magic != IMG_MAGIC) return 0;
    procs();
    if(im->layer_input_id && (target==0x8d65 || target==GL_TEXTURE_2D_) && layer_input_sample(im,target)) return 1;
    if (im->format == VIDEO_YV12) {
        int ok = 0;
        if (target == 0x8d65 || target == GL_TEXTURE_2D_) {
            uint8_t *rgba = rgba_pixels((size_t)im->w * im->h * 4);
            ok = rgba && yv12_pixels(im, rgba) && attach_pixels(im, target, rgba, im->w, GL_RGBA_);
        }
        if (!ok) {
            void (*attach)(unsigned,void*) = egl_shim_gl_proc("glEGLImageTargetTexture2DOES");
            if (attach) attach(target, NULL);
        }
        return 1;
    }
    if (target == 0x8d65 && frame_surfaceflinger() && input_mirror(im) && im->vaddr &&
        (im->format == 1 || im->format == 2 || im->format == 5) && im->stride >= im->w &&
        attach_pixels(im, target, im->vaddr, im->stride, im->format == 5 ? GL_BGRA_EXT_ : GL_RGBA_))
        return 1;
    if (im->has_tex && (uintptr_t)img == (uintptr_t)im->tex) {
        if (im->needs_upload) img_upload_pending(im);
        int bound = 0;
        if (gl_get_integerv) gl_get_integerv(GL_TEXTURE_BINDING_2D_, &bound);
        if (bound && (unsigned)bound != im->tex) img_upload_to(im, target, (unsigned)bound);
        if (target == GL_TEXTURE_2D_) track_bitmap(im, (unsigned)bound);
        return 1;
    }
    int bound = 0;
    if (gl_get_integerv) gl_get_integerv(GL_TEXTURE_BINDING_2D_, &bound);
    img_upload_to(im, target, (unsigned)bound);
    /* HWUI 这条老路径：它自己建纹理、把 image 内容灌进去、然后拿这个纹理当渲染目标。
     * 记下来，回写时才有目标（否则窗口永远黑）。 */
    if (bound > 0 && target==GL_TEXTURE_2D_) {
        void *ctx=cur_ctx();
        /* A GL name may be rebound to another image in the same context,
         * including when a new virtual display replaces an old one. */
        if(encoder_bindings) for(int i=0;i<g_nimg;i++) {
            img_t *old=g_imgs[i];
            if(old!=im && old->encoder_bound && old->caller_ctx==ctx && old->caller_tex==(unsigned)bound) {
                old->encoder_bound=0; old->has_caller_tex=0; --encoder_bindings;
                rt_remove(old);
            }
        }
        track_bitmap(im, (unsigned)bound);
        im->caller_ctx=ctx;
        im->caller_tex = (unsigned)bound;
        im->has_caller_tex = 1;
        im->dirty = 0;
        if(im->encoder && !im->encoder_bound) {
            im->encoder_bound=1;
            ++encoder_bindings;
            __android_log_print(ANDROID_LOG_INFO,"frame-encoder","bound %dx%d name=%d",im->w,im->h,bound);
        }
    }
    return 1;
}

/* On the recording path RenderEngine can retain the FBO while replacing its
 * image. Resolve the actual draw attachment at the time it is rendered. */
void egl_t2_write_encoder(void)
{
    if (!encoder_bindings && !client_target_outputs) return;
    LOCK_IMAGES();
    static void (*attachment)(unsigned,unsigned,unsigned,int*);
    if(!attachment) attachment=egl_shim_gl_proc("glGetFramebufferAttachmentParameteriv");
    if(!attachment || !gl_bind_framebuffer || !gl_bind_buffer || !gl_read_pixels ||
       !gl_get_integerv || !gl_pixel_storei) return;
    int draw=0,tex=0,type=0;
    gl_get_integerv(0x8ca6,&draw);
    if(!draw) return;
    attachment(0x8ca9,GL_COLOR_ATTACHMENT0_,0x8cd0,&type);
    if(type!=0x1702) return;
    attachment(0x8ca9,GL_COLOR_ATTACHMENT0_,0x8cd1,&tex);
    void *ctx=cur_ctx();
    for(int i=0;i<g_nimg;i++) {
        img_t *im=g_imgs[i];
        if(im->client_target_id && ((im->has_tex && im->tex==(unsigned)tex && im->ctx==ctx) ||
           (im->has_caller_tex && im->caller_tex==(unsigned)tex && im->caller_ctx==ctx)))
            im->client_target_rendered=1;
        if(!im->encoder_bound || !im->vaddr || im->caller_tex!=(unsigned)tex ||
           im->caller_ctx!=ctx || im->stride<im->w) continue;
        int read_fbo=0,pbo=0,pack[4];
        const unsigned names[4]={0x0d05,0x0d02,0x0d03,0x0d04};
        gl_get_integerv(0x8caa,&read_fbo);
        gl_get_integerv(0x88ed,&pbo);
        for(int j=0;j<4;j++) gl_get_integerv(names[j],&pack[j]);
        if(read_fbo!=draw) gl_bind_framebuffer(0x8ca8,draw);
        if(pbo) gl_bind_buffer(0x88eb,0);
        gl_pixel_storei(names[0],4); gl_pixel_storei(names[1],im->stride);
        gl_pixel_storei(names[2],0); gl_pixel_storei(names[3],0);
        gl_read_pixels(0,0,im->w,im->h,GL_RGBA_,GL_UNSIGNED_BYTE_,im->vaddr);
        for(int j=0;j<4;j++) gl_pixel_storei(names[j],pack[j]);
        if(pbo) gl_bind_buffer(0x88eb,pbo);
        if(read_fbo!=draw) gl_bind_framebuffer(0x8ca8,read_fbo);
        if(!im->encoder_written)
            __android_log_print(ANDROID_LOG_INFO,"frame-encoder","write %dx%d name=%d fbo=%d",im->w,im->h,tex,draw);
        im->encoder_written=1;
        return;
    }
}
