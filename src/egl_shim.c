/*
 * egl_shim.c —— bionic 侧 EGL 转发层（含 present）
 *
 * 目标：普通 bionic（Android/NDK）程序 dlopen libEGL_d3d12.so 后，
 *       用标准 EGL 调用就能拿到宿主 GPU 上的 GLES 上下文，并把画面取回来。
 *
 * 机制：每个 eglXxx 都把调用转发给 loader 拉起来的 glibc Mesa
 *       （libEGL.so.1.0.0 + libgallium）。EGL 的 Display/Config/Context/
 *       Surface 在 x86-64 上都是指针，两个 ABI 之间可以原样透传；
 *       EGLint/EGLBoolean/EGLenum 都是 32 位；EGLAttrib 都是 64 位。
 *
 * present：真正的 Android window surface 是 bionic 的 ANativeWindow，
 *       glibc Mesa 不认识它。所以 window surface 在这层被"翻译"成
 *       同样尺寸的 pbuffer；eglSwapBuffers 时把 pbuffer 用 glReadPixels
 *       取回来（GPU 侧一次 readback），再写进目标 buffer：
 *         - 有 ANativeWindow → ANativeWindow_lock / unlockAndPost（Waydroid 路径）
 *         - 有测试目标 buffer → egl_shim_set_target 设的那块内存（性能测量路径）
 *       glReadPixels 是 bottom-up，目标 buffer 是 top-down，写的时候翻行。
 */
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <android/native_window.h>
#include <android/rect.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <android/log.h>
#include <pthread.h>
#include <sys/system_properties.h>

static const char *proc_name(void);

extern void *egl_img_create(unsigned int target, void *buffer, const void *attribs);
extern int   egl_img_destroy(void *img);
extern int   egl_img_target_texture(unsigned int target, void *img);

/* ---- 方案 B：present 桥（第 40 轮）----
 * app 仍用我们的 Mesa 渲进 pbuffer；present 时借原版 ANGLE 的窗口 surface 入队，
 * 绕开 HWUI 已把窗口按 API_EGL 连接、导致 ANativeWindow_lock 永远 -22 的问题。 */
extern int   ab_enabled(void);
extern void *ab_view_create(void *window, int w, int h);
extern int   ab_view_frame(void *st, const unsigned char *rgba, int w, int h);
extern void  ab_view_destroy(void *st);
/* 线程局部"正在 ANGLE 初始化"：此时把 GL/EGL 入口指针还给 ANGLE 自己
 * （见 egl_present_angle.c 的注释）。 */
extern int   ab_is_busy(void);
extern void *ab_angle_gles_proc(const char *n);
extern void  egl_img_flush_all(void);

/* 第 61 轮：eglQueryContext 的 shim 转发入口（定义在文件尾部，这里前向声明）*/
extern uintptr_t shim_fwd_eglQueryContext(uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                                          uintptr_t, uintptr_t, uintptr_t, uintptr_t);

extern int   glibc_ldr_init(void);
extern void *glibc_ldr_display(void);
extern void *glibc_ldr_proc(const char *name);

/* 第 57 轮：pid + 进程名一起打，boot 期能一眼分辨 launcher / systemui / SF。 */
static const char *proc_name(void) {
    static char nm[64];
    if (nm[0]) return nm;
    nm[0] = '?'; nm[1] = 0;
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd >= 0) {
        char b[64];
        ssize_t n = read(fd, b, sizeof b - 1);
        close(fd);
        if (n > 0) {
            b[n] = 0;
            for (ssize_t i = 0; i < n; i++) if (b[i] == '\0') { b[i] = 0; break; }
            snprintf(nm, sizeof nm, "%s", b);
        }
    }
    return nm;
}

/* ---- zygote 里绝对不能初始化 ----
 * Android 的 zygote 在预加载阶段就会 dlopen libEGL/libGLESv2 并【真的】调
 * eglGetDisplay + eglInitialize（为了省 app 启动时的驱动初始化）。我们的驱动
 * 一初始化就会把 UMD 拉起来、并创建 libd3d12core 的工作线程；而 zygote 在
 * forkSystemServer 之前的 PreZygoteFork 里要求自己单线程，否则 ART 直接 abort
 * （实测：zygote 反复退出 status 134，system_server 起不来，整机白屏）。
 * 所以：进程名里带 zygote 时一律返回 EGL_NO_DISPLAY，让每个 app / SF 自己
 * 在各自的进程里（线程不受限制）做第一次真正初始化。 */
static int g_is_zygote = -1;
static int shim_is_zygote(void) {
    if (g_is_zygote >= 0) return g_is_zygote;
    g_is_zygote = 0;
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd >= 0) {
        char b[256];
        ssize_t n = read(fd, b, sizeof b - 1);
        close(fd);
        if (n > 0) { b[n] = 0; if (strstr(b, "zygote")) g_is_zygote = 1; }
    }
    return g_is_zygote;
}

/* ================= 进程级分发（第 38 轮） =================
 * 开关文件：/data/local/tmp/angle_apps（不存在就完全保持旧行为）
 *   surfaceflinger 进程 -> 我们的 D3D12/Mesa 栈
 *   其它进程            -> 原样转发给系统自带 ANGLE 的副本（lib*_angle_orig.so）
 *
 * 为什么必须分发：HWUI 的 ReliableSurface 会先 native_window_api_connect(API_EGL)，
 * 我们的 present 走 ANativeWindow_lock() —— 它内部 connect(API_CPU)，BufferQueueProducer
 * 对"已经连了别的 API"直接返回 -22。结果 app 的 buffer 一个都进不了 SF：
 * 所有层 invisible "nothing to draw"，SF 不合成、frame-counter=0（第 37 轮）。
 * ANGLE 用 EGL 路径入队，没有这个冲突。 */
static void *g_angle_egl, *g_angle_gles2, *g_angle_gles1;
static int g_dispatch = -1;
/* 第 39 轮：dispatch 模式下，ANGLE 的 EGL 在 eglInitialize 之前内部 dispatch 表是空的，
 * 直接转发进它的入口会 tail-jump 到 NULL（HWC 的 tombstone：eglGetError+19 → pc 0）。
 * 所以在 ANGLE 初始化完成前，把无害查询挡在自己这里。 */
static int g_angle_ready;

static int proc_is_surfaceflinger(void) {
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd < 0) return 0;
    char b[256];
    ssize_t n = read(fd, b, sizeof b - 1);
    close(fd);
    if (n <= 0) return 0;
    b[n] = 0;
    return strstr(b, "surfaceflinger") != NULL;
}

static int shim_use_angle(void) {
    if (g_dispatch >= 0) return g_dispatch;
    g_dispatch = 0;
    if (access("/data/local/tmp/angle_apps", R_OK) == 0 && !proc_is_surfaceflinger()) {
        g_angle_egl   = dlopen("libEGL_angle_orig.so", RTLD_NOW | RTLD_LOCAL);
        g_angle_gles2 = dlopen("libGLESv2_angle_orig.so", RTLD_NOW | RTLD_LOCAL);
        g_angle_gles1 = dlopen("libGLESv1_CM_angle_orig.so", RTLD_NOW | RTLD_LOCAL);
        if (g_angle_egl) {
            g_dispatch = 1;
        } else {
            printf("[shim] ANGLE orig dlopen failed (%s); fallback to our stack\n", dlerror());
        }
    }
    return g_dispatch;
}

static void *angle_sym(const char *n) {
    void *p = g_angle_egl ? dlsym(g_angle_egl, n) : NULL;
    if (!p && g_angle_gles2) p = dlsym(g_angle_gles2, n);
    if (!p && g_angle_gles1) p = dlsym(g_angle_gles1, n);
    return p;
}

static void *P(const char *n) {
    if (shim_use_angle()) {
        void *p = angle_sym(n);
        if (p) return p;
    }
    return glibc_ldr_proc(n);
}
#define GLE(n, t) ((t)P(n))

/* GL 入口不走符号名查找（libgallium 不导出 glFinish 之类的公共入口），
   必须经 Mesa 自己的 eglGetProcAddress 拿 dispatch stub。 */
static void *GLP(const char *n) {
    typedef __eglMustCastToProperFunctionPointerType (*F)(const char *);
    static F getproc;
    if (!getproc) getproc = (F)P("eglGetProcAddress");
    void *p = getproc ? (void *)getproc(n) : NULL;
    if (!p) p = P(n);
    return p;
}
static void (*g_real_texsub)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
extern void egl_img_bitmap_subimage(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
static void bitmap_texsubimage2d(unsigned target, int level, int x, int y, int w, int h,
                                 unsigned format, unsigned type, const void *pixels) {
    if (!g_real_texsub) g_real_texsub = GLP("glTexSubImage2D");
    if (!g_real_texsub) return;
    g_real_texsub(target, level, x, y, w, h, format, type, pixels);
    egl_img_bitmap_subimage(target, level, x, y, w, h, format, type, pixels);
}

/* 供 GLES 转发库（libGLESv2_d3d12.so）使用：同一进程内共享 Mesa 的 dispatch stub */
__attribute__((visibility("default")))
void *egl_shim_gl_proc(const char *n) {
    if (ab_is_busy()) {          /* ANGLE 初始化中：给它自己的 GLES 指针 */
        void *p = ab_angle_gles_proc(n);
        if (p) return p;
    }
    if (shim_use_angle()) {
        void *p = g_angle_gles2 ? dlsym(g_angle_gles2, n) : NULL;
        if (!p && g_angle_gles1) p = dlsym(g_angle_gles1, n);
        if (!p) p = angle_sym(n);
        return p;
    }
    if (n && !strcmp(n, "glTexSubImage2D")) return (void *)bitmap_texsubimage2d;
    void *p = GLP(n);
    return p;
}

/* 第 53 轮补充 Q：抓帧入口（定义在下面，eglMakeCurrent 里直接调用）。
 * 不能走 dlsym(RTLD_DEFAULT)：我们的库是被 bionic 的 libEGL 用 RTLD_LOCAL 加载的，
 * 全局符号域里查不到它自己的符号。 */
void egl_shim_capture_frame(EGLSurface surf);
/* 第 59 轮：eglGetProcAddress 的白名单要引用它，定义在后面 */
EGLBoolean eglSwapBuffersWithDamageKHR(EGLDisplay dpy, EGLSurface surf,
                                       const EGLint *rects, EGLint n_rects);

/* ================= present 状态 ================= */

/* 第 57 轮：16 个槽在 boot 期可能不够 —— launcher 一次就建 7~9 个窗口 surface
 * （图标 20x20 / 68x68、主窗口、taskbar…），HWUI 的 ReliableSurface 还会再多一份；
 * 超了 surf_add 返回 NULL，present 就查不到 surface、静默走 unmanaged。 */
#define SHIM_MAX_SURF 64

typedef struct {
    EGLSurface egl_surf; /* 真正交给 glibc Mesa 的 pbuffer 句柄，同时当 key */
    EGLint     w, h;
    int        is_window;
    ANativeWindow *win;
    int owns_window_connection;
    unsigned frame_serial, frame_verified;
    int layer_input_ready,layer_input_gpu,layer_input_visible,layer_input_w,layer_input_h; unsigned layer_input_token; int application_app_sampled;
    int previous_shared, capture_trace;
    unsigned captures, skipped, direct_after_capture, history_uses, grabbed, fresh, swaps;
    unsigned window_retry_retry_legacy; /* Early GPU allocation failed on this surface. */
    void      *ab;      /* 方案 B：ANGLE present 桥的每窗口状态（NULL = 没用） */
    EGLDisplay dpy;     /* 第 53 轮补充 I：swap 时自救绑 context 用 */
    EGLConfig  cfg;
    /* 第 53 轮补充 L：最后一帧非空内容。
     * HWUI 只在 damage 区域画，有时候整帧读回来是空的；这时如果直接用空帧覆盖，
     * 图层就会从"有内容"变成"全黑"（用户看到的就是闪/突然黑掉）。
     * 保留上一帧非空内容，空帧时回放它。 */
    unsigned char *last;
    size_t         last_sz;
} shim_surf_t;

static shim_surf_t g_surf[SHIM_MAX_SURF];
static int g_nsurf;

/* 第 60 轮：跨 surface 的「上一帧」缓存。
 * launcher 的窗口在 boot 期每秒重建一次，per-surface 的 last 会随 surface 销毁一起丢，
 * 空帧就没东西可回放 → 写透明黑 → 桌面闪黑。改成全局，只要尺寸相同就复用。 */
/* 全局上一帧缓存已撤销（怀疑多线程 free/malloc 竞态导致 launcher 崩溃）。 */

static void *g_target_bits;
extern void *glibc_ldr_display(void);
static int   g_target_stride;
static int   g_target_w, g_target_h;

#include "scratch.h"

static shim_surf_t *surf_find(EGLSurface s) {
    for (int i = 0; i < g_nsurf; i++)
        if (g_surf[i].egl_surf == s) return &g_surf[i];
    return NULL;
}

static shim_surf_t *surf_add(EGLSurface s, EGLint w, EGLint h, int is_window, ANativeWindow *win) {
    if (!s || g_nsurf >= SHIM_MAX_SURF) {
        static int full_n;
        if (full_n < 8) {
            full_n++;
            __android_log_print(ANDROID_LOG_WARN, "d3d12shim", "surf_add FULL pid=%d comm=%s nsurf=%d surf=%p %dx%d win=%p",
               (int)getpid(), proc_name(), g_nsurf, (void *)s, w, h, (void *)win);
        }
        return NULL;
    }
    shim_surf_t *e = &g_surf[g_nsurf++];
    /* [r71 P1 补丁] 这里以前只赋 5 个字段，last / last_sz / dpy / cfg 会继承上一任槽位。
     * surf_del 用「尾部覆盖」腾槽位时又不给被腾出的尾槽清零，下一次 surf_add 复用它会
     * 让两个槽指向同一块 last -> present 里 free(e->last) 变成 double free
     * （Scudo invalid chunk state，systemui 下拉 QS 偶发崩溃）。必须整体清零。 */
    memset(e, 0, sizeof(*e));
    e->egl_surf = s; e->w = w; e->h = h; e->is_window = is_window; e->win = win; e->ab = NULL;
    if (is_window) {
        char trace[PROP_VALUE_MAX];
        e->capture_trace=__system_property_get("debug.waydroid.acc_trace",trace)>0 && trace[0]=='1';
    }
    return e;
}

static void capture_report(const shim_surf_t *e) {
    if (e->capture_trace && e->swaps)
        __android_log_print(ANDROID_LOG_INFO,"acc-capture",
            "surface=%p %dx%d swaps=%u read=%u skipped=%u direct_after_read=%u replay_read=%u",
            e->egl_surf,e->w,e->h,e->swaps,e->captures,e->skipped,e->direct_after_capture,e->history_uses);
}

static void surf_del(EGLSurface s) {
    int i = -1;
    for (int k = 0; k < g_nsurf; k++)
        if (g_surf[k].egl_surf == s) { i = k; break; }
    if (i < 0) return;
    /* [r71 P1 补丁] 先释放本槽的帧缓存，再搬尾槽过来，最后把腾出的尾槽清零。
     * 以前只做 g_surf[i] = g_surf[--g_nsurf]：
     *   - 被删槽的 last 泄漏；
     *   - 尾槽残渣原样留着，下次 surf_add 复用时会和搬走的槽共享同一个 last -> double free。 */
    capture_report(&g_surf[i]);
    free(g_surf[i].last);
    int last = --g_nsurf;
    if (i != last) g_surf[i] = g_surf[last];
    memset(&g_surf[last], 0, sizeof(g_surf[0]));
}

/* Android 侧只有这些入口需要，用 dlopen 拿，避免对 libandroid 的硬依赖 */
static int (*p_ANativeWindow_lock)(ANativeWindow *, ANativeWindow_Buffer *, ARect *);
static int (*p_ANativeWindow_unlockAndPost)(ANativeWindow *);
static int (*p_ANativeWindow_getWidth)(ANativeWindow *);
static int (*p_ANativeWindow_getHeight)(ANativeWindow *);
static int g_android_probed;

static void probe_android(void) {
    if (g_android_probed) return;
    g_android_probed = 1;
    void *h = dlopen("libandroid.so", RTLD_NOW);
    if (!h) { printf("[shim] libandroid.so not found, window path disabled\n"); return; }
    p_ANativeWindow_lock = (int (*)(ANativeWindow *, ANativeWindow_Buffer *, ARect *))dlsym(h, "ANativeWindow_lock");
    p_ANativeWindow_unlockAndPost = (int (*)(ANativeWindow *))dlsym(h, "ANativeWindow_unlockAndPost");
    p_ANativeWindow_getWidth = (int (*)(ANativeWindow *))dlsym(h, "ANativeWindow_getWidth");
    p_ANativeWindow_getHeight = (int (*)(ANativeWindow *))dlsym(h, "ANativeWindow_getHeight");
}

/* 把 src(bottom-up, 紧排) 写进 dst(top-down, stride 可能带 padding) */
static void blit_flip(const unsigned char *src, int w, int h,
                      unsigned char *dst, int stride) {
    size_t row = (size_t)w * 4;
    for (int y = 0; y < h; y++)
        memcpy(dst + (size_t)(h - 1 - y) * (size_t)stride, src + (size_t)y * row, row);
}

#include "direct.h"
#include "window.h"
/* shared_export_readbuffer and glReadPixels both use the default read
 * renderbuffer. Only substitute a tight client-memory RGBA read. */
static int quickstep_read_is_tight(int width)
{
    typedef const unsigned char *(*GetStringFn)(unsigned);
    typedef void (*GetIntegerFn)(unsigned,int *);
    GetStringFn string=(GetStringFn)GLP("glGetString");
    GetIntegerFn integer=(GetIntegerFn)GLP("glGetIntegerv");
    const char *version=string ? (const char *)string(0x1f02) : NULL;
    if (!integer || !version || strncmp(version,"OpenGL ES 3.",12)) return 0;
    int pbo=-1, row=-1, skip_rows=-1, skip_pixels=-1, read_fb=-1;
    integer(0x88ed,&pbo);         /* GL_PIXEL_PACK_BUFFER_BINDING */
    integer(0x0d02,&row);         /* GL_PACK_ROW_LENGTH */
    integer(0x0d03,&skip_rows);
    integer(0x0d04,&skip_pixels);
    integer(0x8caa,&read_fb);     /* GL_READ_FRAMEBUFFER_BINDING */
    return !read_fb && !pbo && (!row || row==width) && !skip_rows && !skip_pixels;
}

static int present(EGLDisplay dpy, shim_surf_t *e) {
    int direct=0;
    int zero_input = 0;

    static void (*glFinish_)(void);
    static void (*glReadPixels_)(int, int, int, int, unsigned, unsigned, void *);
    static void (*glPixelStorei_)(unsigned, int);
    if (!glFinish_) glFinish_ = (void (*)(void))GLP("glFinish");
    if (!glReadPixels_) glReadPixels_ =
        (void (*)(int, int, int, int, unsigned, unsigned, void *))GLP("glReadPixels");
    if (!glPixelStorei_) glPixelStorei_ = (void (*)(unsigned, int))GLP("glPixelStorei");
    if (!glFinish_ || !glReadPixels_ || !glPixelStorei_) {
        static int miss;
        if (miss < 3) { miss++;
            __android_log_print(ANDROID_LOG_WARN, "d3d12shim", "present: gl proc MISSING finish=%p readpix=%p pixelstore=%p",
               (void *)glFinish_, (void *)glReadPixels_, (void *)glPixelStorei_); }
        return EGL_FALSE;
    }

    int w = e->w, h = e->h;
    int allow_clear = e->application_app_sampled && e->layer_input_w == w && e->layer_input_h == h;
    struct sb_slot *sbs __attribute__((cleanup(release_scratch))) = NULL;
    unsigned char *scratch = NULL;
    int scratch_failed = 0;
    size_t need = (size_t)w * (size_t)h * 4;
    /* 先把 app 的绘制队列冲出去（这时可能还没有 surface，glFinish 也不会崩） */
    glFinish_();
    glPixelStorei_(0x0D05 /*GL_PACK_ALIGNMENT*/, 4);
    /* 第 58 轮（P0 收口）：PACK_ROW_LENGTH 从来没归零过。
     * 如果 app 之前把它设成非 0（Skia 读回时会设），我们这次 glReadPixels
     * 就会按错的步长排布 —— 实测读回只剩 1/4 的像素，其余变 (0,0,0,0)。
     * 同时把 SKIP_ROWS/SKIP_PIXELS 也归零。 */
    glPixelStorei_(0x0D02 /*GL_PACK_ROW_LENGTH*/, 0);
    glPixelStorei_(0x0D03 /*GL_PACK_SKIP_ROWS*/, 0);
    glPixelStorei_(0x0D04 /*GL_PACK_SKIP_PIXELS*/, 0);

    /* 还原用（读完像素后把 EGL 状态还给 app） */
    static __thread EGLContext t_rctx;
    static __thread EGLSurface t_rsurf;
    static __thread EGLDisplay t_rdpy;
    static __thread void *t_rmc;
    static __thread int t_rneed;

    /* 第 53 轮补充 I（关键）：apps 在 eglSwapBuffers 时**往往已经没有 current context**
     * （实测 cur=0x0 swap=0x...，same=0），这时 glFinish/glReadPixels 什么都不会做，
     * 读回来就是全 0 —— 这就是"窗口里只有壁纸、应用层全黑"的真因。
     * 这里在自己名下临时绑一个 context 到这个 pbuffer，读完再解绑。 */
    /* 第 56 轮：每帧诊断要把“swap 时的 current state”和 nz 打在同一行，
     * 所以把这两个值提到外层作用域。 */
    int d_bind = -1;   /* 本帧自救绑定：-1 没试 / 0 失败 / 1 成功 */
    /* 第 63 轮：present 之前，本线程的 EGL 状态是什么。
     * hwui 是在「自己的 context 还绑着」的前提下调用 eglSwapBuffers 的，
     * 所以 present 结束时必须把状态还原，而不是一律解绑。 */
    extern void *egl_shim_thread_ctx(void);
    extern void *egl_shim_thread_draw(void);
    void *prev_ctx  = egl_shim_thread_ctx();
    void *prev_surf = egl_shim_thread_draw();
    {
        typedef EGLContext (*GC)(void);
        typedef EGLBoolean (*MC)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
        typedef EGLSurface (*GCUR)(EGLint);
        /* 注意：必须用 EGLF（glibc EGL 的入口）而不是 GLE（Mesa 的 eglGetProcAddress），
         * 后者对 eglMakeCurrent/eglCreateContext 这些核心函数返回 NULL。 */
        GC gc = GLE("eglGetCurrentContext", GC);
        MC mc = GLE("eglMakeCurrent", MC);
        GCUR gcur = GLE("eglGetCurrentSurface", GCUR);
        EGLContext cctx = gc ? gc() : EGL_NO_CONTEXT;
        EGLSurface csurf = gcur ? gcur(0x3058 /*EGL_DRAW*/) : EGL_NO_SURFACE;
        /* 实测 apps 在 swap 时是"有 context 但没有 current surface"（cur=0x0），
         * 这种状态下 glReadPixels 同样读不到东西，所以两个条件都要判。 */
        /* 第 56 轮修正：apps 在 swap 时常常"有 context 但没有 surface"（实测 cur=0x0）。
         * 第 55 轮的做法是**自建一个 context** 绑上去读，但实测那样读出来是全 0：
         *   stage2/r56/test_readback.c mode1（画完 → eglMakeCurrent(NO_SURFACE) → swap）
         *   稳定复现 readback 全 0；mode2（swap 前把 app 自己的 context+surface 绑回去）就正常。
         * 所以优先用 app 自己的 context（cctx）绑回这块 pbuffer，
         * 只有 cctx 也为空时才退回自建 context。 */
        if (mc) {
            typedef EGLDisplay (*GD)(void);
            EGLDisplay pdpy = e->dpy;
            if (!pdpy) { GD gd = GLE("eglGetCurrentDisplay", GD); if (gd) pdpy = gd(); }
            static EGLContext own_ctx;
            static EGLConfig  own_cfg;
            static EGLDisplay own_dpy;
            /* 第 60 轮：own_ctx 不再只在"没有 app context"时才建。
             * 实测 app 的 context 已经 current、但 READ surface 为空时，重绑也修不回来；
             * 而全新 context 一绑上就能读回洋红。所以只要缺 own_ctx 就预建一个备用。 */
            if (pdpy && e->cfg) {
                if (!own_ctx || own_cfg != e->cfg || own_dpy != pdpy) {
                    typedef EGLContext (*CC)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
                    CC cc = GLE("eglCreateContext", CC);
                    if (cc) {
                        const EGLint attr[] = { 0x3098 /*EGL_CONTEXT_CLIENT_VERSION*/, 2, EGL_NONE };
                        EGLContext c = cc(pdpy, e->cfg, EGL_NO_CONTEXT, attr);
                        if (c) { own_ctx = c; own_cfg = e->cfg; own_dpy = pdpy; }
                    }
                }
            }
            /* 第 60 轮（实验）：优先用 shim 自己的 context 读回。
             * boot 期创建的 app context 在这块 surface 上 clear/draw 都不落像素，
             * 而全新 context 一绑上去就能写能读 —— 说明坏的是 app 那个 context 的
             * default framebuffer，不是 surface。改用 own_ctx 读，就能判断
             * "app 到底有没有把内容画进这块 buffer"。 */
            /* 第 60 轮（进程白名单）：own_ctx 只给 launcher 用。
             * 无条件让所有进程走 own_ctx，实测会把 surfaceflinger 的
             * RenderEngine 带崩（16:33:28 SIGSEGV，之后 zygote/SF 崩溃循环，
             * 环境整个坏掉，连"重启 launcher 就正常"的对照都做不了）。
             * 白名单只放 launcher；别的进程仍用 app 自己的 context。 */
            static int launcher_proc = -1;
            if (launcher_proc < 0) {
                char cb[64] = { 0 };
                int fdc = open("/proc/self/comm", O_RDONLY);
                if (fdc >= 0) {
                    int nrd = (int)read(fdc, cb, sizeof cb - 1);
                    close(fdc);
                    if (nrd > 0) { char *nl = strchr(cb, '\n'); if (nl) *nl = 0; }
                }
                launcher_proc = (strstr(cb, "launcher3") != NULL);
            }
            
            EGLContext bind_ctx = own_ctx && launcher_proc ? own_ctx : cctx;
            if (bind_ctx && pdpy) {
                EGLBoolean ok;
                /* 第 60 轮修正（SENTINEL + MAGENTA 之后）：**完全不解绑，也不看 read surface**。
                 * 三条实测把这里钉死了：
                 *   - SENTINEL（gles 侧）：draw 之后同一 framebuffer 里 glReadPixels 读得到像素
                 *     （(10,10)=000080bf 这类），说明 draw 确实落了；
                 *   - MAGENTA（egl 侧）：present 里用当前绑定清洋红也读得回来，说明读到的是这块 pbuffer；
                 *   - 但 eglGetCurrentSurface(EGL_READ) 在这块 pbuffer 上恒为 0。
                 * 因此"read=0 就解绑重绑"的旧逻辑每次都触发，而 eglMakeCurrent(NULL) + 重绑
                 * 会让 Mesa 重建默认 framebuffer，把 app 刚画的那一帧丢掉 —— 这就是 nz=0 的来源。
                 * 现在只绑一次，什么都不检查。 */
                ok = mc(pdpy, e->egl_surf, e->egl_surf, bind_ctx);
                d_bind = ok ? 1 : 0;
                if (ok) {
                    /* 绑上之后立刻冲一次：app 的绘制可能还在该 context 的队列里，
                     * 只有 context 真正绑到这块 surface 上时才落进 pbuffer。 */
                    glFinish_();
                    t_rneed = 1;
                    t_rctx = cctx; t_rsurf = csurf; t_rdpy = pdpy; t_rmc = (void *)mc;
                }
            }
        }
    }

    int saved_rfb = -1, saved_dfb = -1;
    {
        typedef void (*BF)(unsigned, unsigned);
        typedef void (*GI)(unsigned, int *);
        BF bf = (BF)GLP("glBindFramebuffer");
        GI gi = (GI)GLP("glGetIntegerv");
        if (bf && gi) {
            gi(0x8CAA /*GL_READ_FRAMEBUFFER_BINDING*/, &saved_rfb);
            gi(0x8CA6 /*GL_DRAW_FRAMEBUFFER_BINDING*/, &saved_dfb);
            if (saved_rfb != 0) bf(0x8CA8 /*GL_READ_FRAMEBUFFER*/, 0);
            if (saved_dfb != 0) bf(0x8CA9 /*GL_DRAW_FRAMEBUFFER*/, 0);
        } else {
            saved_rfb = saved_dfb = -1;
        }
    }
    {
        int confirmed_empty=0;
        direct=layer_input_submit(e,w,h,&confirmed_empty);
        e->previous_shared=direct;
        if(confirmed_empty) {
            zero_input = d_bind == 1 && quickstep_read_is_tight(w);
        } else if(!direct) {
            direct=frame_submit(e,w,h,glReadPixels_);
        }
        if (e->capture_trace) {
            ++e->swaps;
            if (direct) e->direct_after_capture+=e->fresh;
            e->fresh=0;
            if (!(e->swaps % 60)) capture_report(e);
        }
        if(direct) goto frame_restore_fbo;
        sbs = acquire_scratch(e->win, w, h);
        if (!sbs) {
            scratch_failed = 1;
            goto frame_restore_fbo;
        }
        scratch = sbs->b[sbs->cur];
        if (zero_input)
            memset(scratch,0,need);
        else
            glReadPixels_(0, 0, w, h, 0x1908 /*GL_RGBA*/, 0x1401 /*GL_UNSIGNED_BYTE*/, scratch);
    }
frame_restore_fbo:
    {
        typedef void (*BF)(unsigned, unsigned);
        BF bf = (BF)GLP("glBindFramebuffer");
        if (bf && saved_rfb >= 0) {
            if (saved_rfb != 0) bf(0x8CA8, (unsigned)saved_rfb);
            if (saved_dfb != 0) bf(0x8CA9, (unsigned)saved_dfb);
        }
    }

    if (direct || scratch_failed) goto frame_restore_context;
    /* 第 53 轮补充 L：空帧回放上一帧（见 shim_surf_t.last 的注释） */
    if (!allow_clear) {
        size_t i = 0;
        while (i < need && !scratch[i]) i += 101;
        if (i >= need) {
            if (e->last && e->last_sz == need) {
                if (e->capture_trace && e->grabbed) ++e->history_uses;
                memcpy(scratch, e->last, need);
            }
        } else {
            if (!e->last || e->last_sz != need) {
                free(e->last);
                e->last = (unsigned char *)malloc(need);
                e->last_sz = e->last ? need : 0;
            }
            if (e->last) { memcpy(e->last, scratch, need); e->grabbed=0; }
        }
    }

    /* Only the EGL window's default color buffer contains its presented frame.
     * The last user FBO can hold an unrelated mask or intermediate render. */
    /* 第 53 轮补充 J：把 EGL 状态还给 app（读之前我们抢用了它） */
frame_restore_context:
    if (t_rneed && t_rmc) {
        typedef EGLBoolean (*MC)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
        if (t_rsurf != EGL_NO_SURFACE) {
            ((MC)t_rmc)(t_rdpy, t_rsurf, t_rsurf, t_rctx);
        }
        t_rneed = 0;
    }

    if (direct || scratch_failed) goto present_done;
    
    {
        unsigned *px = (unsigned *)scratch;
        size_t an = need / 4;
        for (size_t i = 0; i < an; i++) {
            unsigned v = px[i];
            if ((v & 0xFFFFFF) && (v >> 24) == 0) { px[i] = v | 0xFF000000u; }
        }
    }

        /* 目标 1：测试缓冲 */
    if (g_target_bits && g_target_w >= w && g_target_h >= h) {
        blit_flip(scratch, w, h, (unsigned char *)g_target_bits, g_target_stride);
    }

    /* 目标 2a：方案 B —— 借原版 ANGLE 的窗口 surface 把这一帧入队。
     * 进桥前先记下我们自己的 context，回来立刻 make current 还原，
     * 否则 app 后续的 Mesa 调用会打到 ANGLE 的 context 上。 */
    if (e->win && ab_enabled()) {
        typedef EGLContext (*GC)(void);
        typedef EGLBoolean (*MC)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
        GC gc = GLE("eglGetCurrentContext", GC);
        MC mc = GLE("eglMakeCurrent", MC);
        EGLContext ours = gc ? gc() : EGL_NO_CONTEXT;
        if (!e->ab) {
            e->ab = ab_view_create((void *)e->win, w, h);
        }
        int bok = e->ab ? ab_view_frame(e->ab, scratch, w, h) : 0;
        if (mc) mc(dpy, e->egl_surf, e->egl_surf, ours);
        if (bok) {
            return EGL_TRUE;
        }
    }

    /* [r76] 首帧 cancel / NOCOMMIT 两条路线已验证不可用（见 stage3/r76/RESULT.md）：
     * eglSwapBuffers 返回成功而实际不提交，会让 HWUI 认为该帧已完成、不再重绘，
     * 静态窗口（launcher 主窗口）会永久空白。此路不用。 */

    if (e->win && p_ANativeWindow_lock && p_ANativeWindow_unlockAndPost) {
        ANativeWindow_Buffer buf;
        memset(&buf, 0, sizeof buf);
        int lrc = p_ANativeWindow_lock(e->win, &buf, NULL);
        if (lrc != 0) {
            /* 第 48 轮：libgui 的 BufferQueueProducer::connect 已 patch 成允许重复连接。
             * 这里主动 connect(API_CPU) 再 lock —— patch 生效的话这一步应该返回 0，
             * 而不是之前的 -22。 */
            /* 用导出的 ANativeWindow_perform，不要猜结构偏移。
             * NATIVE_WINDOW_CONNECT=1，NATIVE_WINDOW_API_CPU=1。
             * 注意：op=4 是 SET_BUFFER_COUNT，不是 CONNECT（之前写错了）。 */
            typedef int (*pfn_perform_t)(void *, int, ...);
            static pfn_perform_t p_perform;
            if (!p_perform) {
                void *hl = dlopen("libandroid.so", RTLD_NOW);
                p_perform = hl ? (pfn_perform_t)dlsym(hl, "ANativeWindow_perform") : NULL;
                if (!p_perform) {
                    pfn_perform_t pf0 = *(pfn_perform_t *)((char *)e->win + 0x38);
                    p_perform = pf0;
                }
            }
            if (p_perform) {
                int c1 = p_perform(e->win, 1, 1);   /* CONNECT, API_CPU */
                if (c1 == 0) lrc = p_ANativeWindow_lock(e->win, &buf, NULL);
            }
        }
        if (lrc == 0) {
            int cw = buf.width < w ? buf.width : w;
            int ch = buf.height < h ? buf.height : h;
            unsigned char *bits = (unsigned char *)buf.bits;
            size_t row = (size_t)cw * 4;
            /* 第 50 轮：严格边界检查 + 对照开关。
             * 之前的日志里 HWUI 拿到 format=777211904 / 2x223472400 这种垃圾参数，
             * 怀疑是这里越界写坏了堆。 */
            int sane = bits && cw > 0 && ch > 0 && cw <= 16384 && ch <= 16384 &&
                       buf.stride >= cw && buf.stride <= 16384;
            if (sane) {
                int skip_empty = 0;
                {
                    if (!allow_clear) {
                        size_t nzp = 0;
                        for (size_t i = 0; i < need; i += 97) if (scratch[i]) { nzp = 1; break; }
                        if (!nzp) {
                            /* 全 0 帧：如果另一块里还留着同尺寸的有效帧，直接拿它写窗口。
                             * 这样 SF 永远不会拿到“空 buffer”，闪黑消失；
                             * 没有可用上一帧（开机第一帧就是空的）时才退回跳过。 */
                            int other = sbs->cur ^ 1;
                            if (sbs->ok[other] && sbs->bw[other] == w && sbs->bh[other] == h) {
                                scratch = sbs->b[other];
                            } else if (sbs->ok[other] && sbs->bw[other] > 0 && sbs->bh[other] > 0) {
                                /* 尺寸变了（窗口在启动/收起动画、或重建）：
                                 * 同尺寸回填用不了，但也不能整块交白卷 —— 那正是
                                 * "动画中窗口变透明、只剩顶部白条"的成因。
                                 * 这里把上一帧按两者较小尺寸贴到左上角，其余保留 buffer 原内容。 */
                                int ow = sbs->bw[other], oh = sbs->bh[other];
                                int ww = ow < cw ? ow : cw;
                                int hh = oh < ch ? oh : ch;
                                const unsigned char *src = sbs->b[other];
                                for (int yy = 0; yy < hh; yy++)
                                    memcpy(bits + (size_t)(ch - 1 - yy) * (size_t)buf.stride * 4,
                                           src + (size_t)yy * (size_t)ow * 4, (size_t)ww * 4);
                                skip_empty = 1;
                            } else {
                                skip_empty = 1;
                            }
                        } else {
                            /* 本帧有真实内容：标这块有效，并切到另一块做下一帧的落点。
                             * 必须切：否则下一帧会覆盖这块，空帧回填时读到的是刚被清空的内容。 */
                            sbs->ok[sbs->cur] = 1;
                            sbs->bw[sbs->cur] = w;
                            sbs->bh[sbs->cur] = h;
                            sbs->cur ^= 1;
                        }
                    }
                }
                if (!skip_empty)
                for (int y = 0; y < ch; y++)
                    memcpy(bits + (size_t)(ch - 1 - y) * (size_t)buf.stride * 4,
                           scratch + (size_t)y * (size_t)w * 4, row);
            }
            p_ANativeWindow_unlockAndPost(e->win);
        }
    }

present_done:
    
    if (d_bind == 1 && dpy) {
        
        typedef EGLBoolean (*MCF)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
        MCF mcf = GLE("eglMakeCurrent", MCF);
        if (!prev_ctx || !prev_surf) {
            if (mcf) mcf(dpy, NULL, NULL, NULL);
        } else {
            if (mcf) mcf(dpy, prev_surf, prev_surf, prev_ctx);
        }
        d_bind = 0;
    }
    (void)dpy;
    return scratch_failed ? EGL_FALSE : EGL_TRUE;
}

/* ---------------- 测试/集成用的导出 ---------------- */

__attribute__((visibility("default")))
void egl_shim_set_target(void *bits, int stride, int w, int h) {
    g_target_bits = bits; g_target_stride = stride; g_target_w = w; g_target_h = h;
}

/* ---------------- display ---------------- */

EGLDisplay eglGetDisplay(EGLNativeDisplayType dpy) {
    if (shim_use_angle()) {
        typedef EGLDisplay (*F)(EGLNativeDisplayType);
        F a = (F)angle_sym("eglGetDisplay");
        EGLDisplay d = a ? a(dpy) : EGL_NO_DISPLAY;
        return d;
    }
    (void)dpy;
    if (shim_is_zygote()) return EGL_NO_DISPLAY;   /* 见 shim_is_zygote 注释 */
    return (EGLDisplay)glibc_ldr_display();
}

/* Android delegates ANativeWindow ownership when selecting the system ANGLE ABI. */
static EGLDisplay android_angle_display;
static _Thread_local EGLint window_error;
extern int shim_connect_window(ANativeWindow *, int, int);
extern void shim_disconnect_window(ANativeWindow *);

EGLDisplay eglGetPlatformDisplay(EGLenum platform, void *native, const EGLAttrib *attribs) {
    if (shim_use_angle()) {
        typedef EGLDisplay (*F)(EGLenum, void *, const EGLAttrib *);
        F a = (F)angle_sym("eglGetPlatformDisplay");
        EGLDisplay d = a ? a(platform, native, attribs) : EGL_NO_DISPLAY;
        return d;
    }
    (void)platform; (void)native; (void)attribs;
    if (shim_is_zygote()) return EGL_NO_DISPLAY;   /* 见 shim_is_zygote 注释 */
    EGLDisplay display = (EGLDisplay)glibc_ldr_display();
    if (display && platform == 0x3202 /* EGL_PLATFORM_ANGLE_ANGLE */)
        __atomic_store_n(&android_angle_display, display, __ATOMIC_RELEASE);
    return display;
}

EGLBoolean eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor) {
    typedef EGLBoolean (*F)(EGLDisplay, EGLint *, EGLint *);
    /* 第 61 轮（BOOTWAIT）：冷启动时 launcher 的 EGL 初始化发生在系统 boot 完成之前，
     * 之后 Activity 的窗口 surface 要等 4~5 秒才建出来；实测这种"context 先建、主窗口
     * surface 后到"的组合会让整个进程的 draw 静默无输出（GL 无错、framebuffer complete）。
     * 而"延迟 10 秒再初始化"（此时 boot 已完成、窗口 surface 立刻就有）是正常的。
     * 所以这里不是睡固定秒数，而是等 sys.boot_completed=1。（第 62 轮起默认生效，不再需要开关） */

    F f = GLE("eglInitialize", F);
    EGLBoolean r = f ? f(dpy, major, minor) : EGL_FALSE;
    if (r && shim_use_angle()) g_angle_ready = 1;
    return r;
}

extern EGLBoolean egl_img_terminate(EGLDisplay,EGLBoolean (*)(EGLDisplay));
extern EGLBoolean egl_img_destroy_context(EGLDisplay,EGLContext,EGLBoolean (*)(EGLDisplay,EGLContext));
EGLBoolean eglTerminate(EGLDisplay dpy) {
    typedef EGLBoolean (*F)(EGLDisplay);
    F f = GLE("eglTerminate", F);
    EGLBoolean r = f ? egl_img_terminate(dpy,f) : EGL_FALSE;
    return r;
}

const char *eglQueryString(EGLDisplay dpy, EGLint name) {
    typedef const char *(*F)(EGLDisplay, EGLint);
    if (shim_use_angle()) {
        if (!g_angle_ready) return NULL;
        F a = (F)angle_sym("eglQueryString");
        return a ? a(dpy, name) : NULL;
    }
    F f = GLE("eglQueryString", F);
    const char *base = f ? f(dpy, name) : NULL;
    if (name != 0x3055 /* EGL_EXTENSIONS */ || !dpy) return base;

    /* 扩展字符串要"合并"，不能原样转发：Mesa 的 surfaceless 平台不知道
     * EGL_ANDROID_image_native_buffer，但这个扩展我们是真的实现了
     * （egl_image.c：从 gralloc buffer 建纹理、以及把纹理写回 buffer）。
     * SF/HWUI 就是靠这行字符串决定走不走 EGLImage 快速路径，
     * 不宣传等于让它们永远走慢路径。
     *
     * 反过来，EGL_ANDROID_native_fence_sync 【绝对不能】宣传：
     * eglDupNativeFenceFDANDROID 目前还是返回 -1 的桩，宣传出去调用方
     * 会把 -1 当合法 fd 用（跨进程交接直接坏）。caveat 记在 HANDOFF。 */
    static char merged[4096];
    static const char *cached_base;
    static int cached;
    if (!cached || cached_base != base) {
        /* Skia/RenderEngine 决定走不走 EGLImage 快速路径，看的是 EGL_KHR_image_base；
         * 只给 ANDROID_image_native_buffer 不够，它们会绕开 eglCreateImageKHR。 */
        const char *extra =
            "EGL_ANDROID_image_native_buffer "
            "EGL_KHR_image_base EGL_KHR_image EGL_ANDROID_get_native_client_buffer";
        if (base && *base) snprintf(merged, sizeof merged, "%s %s", base, extra);
        else               snprintf(merged, sizeof merged, "%s", extra);
        cached_base = base;
        cached = 1;
    }
    return merged;
}

EGLint eglGetError(void) {
    if (window_error) {
        EGLint error = window_error;
        window_error = 0;
        return error;
    }
    typedef EGLint (*F)(void);
    if (shim_use_angle() && !g_angle_ready) return 0x3000 /* EGL_SUCCESS */;
    F f = GLE("eglGetError", F);
    EGLint e = f ? f() : EGL_NOT_INITIALIZED;
    return e;
}

EGLBoolean eglBindAPI(EGLenum api) {
    typedef EGLBoolean (*F)(EGLenum);
    F f = GLE("eglBindAPI", F);
    return f ? f(api) : EGL_FALSE;
}

EGLenum eglQueryAPI(void) {
    typedef EGLenum (*F)(void);
    F f = GLE("eglQueryAPI", F);
    return f ? f() : EGL_OPENGL_ES_API;
}

/* ---------------- config ---------------- */

/* ---- SURFACE_TYPE 双向翻译（第 23 轮，接 SF 的前置条件）----
 * 我们的 Mesa 是 -Dplatforms= 的 surfaceless 构建，config 只报 EGL_PBUFFER_BIT；
 * 实测：请求 EGL_WINDOW_BIT 或 EGL_WINDOW_BIT|EGL_PBUFFER_BIT 都返回 0 个 config
 * （不写 SURFACE_TYPE 时按规范默认就是 WINDOW_BIT，也是 0 个）。
 * 而 window surface 在转发层本来就被翻成同尺寸 pbuffer，所以：
 *   查询前 → 把 WINDOW/PIXMAP 位换成 PBUFFER 位（缺省时补 PBUFFER）
 *   查询后 → 回读时再把 WINDOW 位加回去，免得 HWUI/SF 认为这 config 不支持窗口。 */
#define SHIM_EGL_WINDOW   0x0004
#define SHIM_EGL_PIXMAP   0x0002
#define SHIM_EGL_PBUFFER  0x0001
#define SHIM_EGL_SURFTYPE 0x3033
#define SHIM_EGL_NONE     0x3038

EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint *attribs,
                           EGLConfig *configs, EGLint n, EGLint *num) {
    typedef EGLBoolean (*F)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
    if (shim_use_angle()) {
        if (!g_angle_ready) return EGL_FALSE;
        F a = (F)angle_sym("eglChooseConfig");
        return a ? a(dpy, attribs, configs, n, num) : EGL_FALSE;
    }
    F f = GLE("eglChooseConfig", F);
    if (!f) return EGL_FALSE;
    EGLint fixed[128];
    int k = 0, saw = 0;
    if (attribs) {
        for (const EGLint *p = attribs; p[0] != SHIM_EGL_NONE && k < 124; p += 2) {
            if (p[0] == SHIM_EGL_SURFTYPE) {
                fixed[k++] = SHIM_EGL_SURFTYPE;
                fixed[k++] = (p[1] & ~(SHIM_EGL_WINDOW | SHIM_EGL_PIXMAP)) | SHIM_EGL_PBUFFER;
                saw = 1;
            } else {
                fixed[k++] = p[0]; fixed[k++] = p[1];
            }
        }
    }
    if (!saw) { fixed[k++] = SHIM_EGL_SURFTYPE; fixed[k++] = SHIM_EGL_PBUFFER; }
    fixed[k++] = SHIM_EGL_NONE;
    EGLBoolean r = f(dpy, fixed, configs, n, num);
    return r;
}

EGLBoolean eglGetConfigs(EGLDisplay dpy, EGLConfig *configs, EGLint n, EGLint *num) {
    typedef EGLBoolean (*F)(EGLDisplay, EGLConfig *, EGLint, EGLint *);
    F f = GLE("eglGetConfigs", F);
    EGLBoolean r = f ? f(dpy, configs, n, num) : EGL_FALSE;
    return r;
}

/* ---- EGL_NATIVE_VISUAL_ID 必须报 Android 的 HAL_PIXEL_FORMAT_* ----
 * 第 35 轮定位：Android 侧这个属性是 HAL 像素格式（ANGLE 报 1 = RGBA_8888），
 * 而 Mesa 的 surfaceless config 报 0。Waydroid 的 hwcomposer 直接拿这个值去
 * create_shm_wl_buffer() 换 wl_shm 格式（只认 1/2/5），拿到 0 就打
 * "Cannot convert hal format to shm format 0" 返回 -EINVAL → 显示缓冲建不出来 →
 * Wayland client 被断开 → HWC abort → SurfaceFlinger 跟着死 → 开机循环、白屏。 */
#define SHIM_EGL_NATIVE_VISUAL_ID 0x302E
#define SHIM_EGL_ALPHA_SIZE       0x3021

EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig cfg, EGLint attr, EGLint *val) {
    typedef EGLBoolean (*F)(EGLDisplay, EGLConfig, EGLint, EGLint *);
    if (shim_use_angle()) {
        if (!g_angle_ready) return EGL_FALSE;
        F a = (F)angle_sym("eglGetConfigAttrib");
        return a ? a(dpy, cfg, attr, val) : EGL_FALSE;
    }
    F f = GLE("eglGetConfigAttrib", F);
    if (!f) return EGL_FALSE;
    EGLBoolean r = f(dpy, cfg, attr, val);
    /* 窗口能力是我们在转发层"补"出来的：回读时把 WINDOW 位加回去 */
    if (r && val && attr == SHIM_EGL_SURFTYPE) *val |= SHIM_EGL_WINDOW;
    if (r && val && attr == SHIM_EGL_NATIVE_VISUAL_ID) {
        EGLint alpha = 0;
        if (f(dpy, cfg, SHIM_EGL_ALPHA_SIZE, &alpha) && alpha > 0) {
            *val = 1;   /* HAL_PIXEL_FORMAT_RGBA_8888 */
        } else {
            *val = 2;   /* HAL_PIXEL_FORMAT_RGBX_8888 */
        }
    }
    return r;
}

/* ---------------- context ---------------- */

EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig cfg, EGLContext share, const EGLint *attribs) {
    typedef EGLContext (*F)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
    F f = GLE("eglCreateContext", F);
    EGLContext c = f ? f(dpy, cfg, share, attribs) : EGL_NO_CONTEXT;
    return c;
}

EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx) {
    typedef EGLBoolean (*F)(EGLDisplay, EGLContext);
    F f = GLE("eglDestroyContext", F);
    EGLBoolean r = f ? egl_img_destroy_context(dpy,ctx,f) : EGL_FALSE;
    return r;
}

/* 第 58 轮：每线程的当前 draw surface。
 * eglGetCurrentSurface 在 GLES 库里查不到（dsurf 一直 0x0），只能自己在 makeCurrent 时记。 */
static __thread EGLSurface g_tls_draw_surf;
/* 第 61 轮（NOMC）：同时记下 app 最后一次 makeCurrent 成功时绑的 ctx。
 * present 里要判断"app 的画此刻是否还绑在这块 surface 上" —— 若还绑着，
 * 再调 eglMakeCurrent 会让 Mesa 重建 default framebuffer，把刚画的那帧丢掉。 */
static __thread EGLContext g_tls_ctx;

__attribute__((visibility("default")))
void *egl_shim_thread_draw(void) { return (void *)g_tls_draw_surf; }

__attribute__((visibility("default")))
void *egl_shim_thread_ctx(void) { return (void *)g_tls_ctx; }

EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
    typedef EGLBoolean (*F)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
    F f = GLE("eglMakeCurrent", F);
    EGLBoolean r = f ? f(dpy, draw, read, ctx) : EGL_FALSE;
    if (r) { g_tls_draw_surf = draw; g_tls_ctx = ctx; }
    return r;
}

EGLBoolean eglReleaseThread(void) {
    typedef EGLBoolean (*F)(void);
    F f = GLE("eglReleaseThread", F);
    EGLBoolean r = f ? f() : EGL_TRUE;
    return r;
}

EGLContext eglGetCurrentContext(void) {
    typedef EGLContext (*F)(void);
    F f = GLE("eglGetCurrentContext", F);
    EGLContext c = f ? f() : EGL_NO_CONTEXT;
    return c;
}

EGLSurface eglGetCurrentSurface(EGLint readdraw) {
    typedef EGLSurface (*F)(EGLint);
    F f = GLE("eglGetCurrentSurface", F);
    return f ? f(readdraw) : EGL_NO_SURFACE;
}

EGLDisplay eglGetCurrentDisplay(void) {
    typedef EGLDisplay (*F)(void);
    F f = GLE("eglGetCurrentDisplay", F);
    return f ? f() : EGL_NO_DISPLAY;
}

/* ---------------- 在 glFinish 里抓帧（第 53 轮补充 O） ----------------
 * 动机：apps 调 eglSwapBuffers 时常常已经"没有 current surface"，我们从外面读不到内容。
 * 而它们画完一定会 glFinish（HWUI 会），那一刻 context/surface 都还在。
 * 所以：glFinish 里把这块窗口 surface 的画面抓到 e->last，present 时如果自己读不到就用它。 */
void egl_shim_capture_frame(EGLSurface surf) {
    shim_surf_t *e = surf_find(surf);
    if (!e || !e->is_window || e->w <= 0 || e->h <= 0) {
        return;
    }
    typedef void (*GI)(unsigned, int *);
    typedef void (*RP)(int, int, int, int, unsigned, unsigned, void *);
    GI gi = (GI)GLP("glGetIntegerv");
    RP rp = (RP)GLP("glReadPixels");
    if (!rp) return;
    int fb = 0;
    if (gi) gi(0x8CA6 /*GL_FRAMEBUFFER_BINDING*/, &fb);
    /* 只在默认 framebuffer 上抓：FBO 里的内容是 app 自己的中间结果 */
    if (fb != 0) return;
    /* This specific fallback intentionally accepts transparent clears and
     * never reads last. First frames, system windows and failed submissions
     * keep the old current-context capture. */
    if (e->previous_shared && e->layer_input_gpu && e->application_app_sampled &&
        e->layer_input_w == e->w && e->layer_input_h == e->h) {
        if (e->capture_trace) ++e->skipped;
        return;
    }
    size_t need = (size_t)e->w * e->h * 4;
    if (!e->last || e->last_sz != need) {
        free(e->last);
        e->last = (unsigned char *)malloc(need);
        e->last_sz = e->last ? need : 0;
    }
    if (!e->last) return;
    /* 第 61 轮：和第 58 轮 present 里的教训一样 —— Skia 会设 PACK_ROW_LENGTH，
     * 读整帧前必须归零，否则读回按错步长排布，统计出假的全 0。
     * 同时先 glFinish，把队列里的 draw 冲出去再读。 */
    {
        typedef void (*PS)(unsigned, int);
        typedef void (*GF2)(void);
        static PS ps;
        static GF2 gf;
        if (!ps) ps = (PS)GLP("glPixelStorei");
        if (!gf) gf = (GF2)GLP("glFinish");
        if (ps) { ps(0x0D05, 4); ps(0x0D02, 0); ps(0x0D03, 0); ps(0x0D04, 0); }
        if (gf) gf();
    }
    rp(0, 0, e->w, e->h, 0x1908, 0x1401, e->last);
    if (e->capture_trace) { ++e->captures; ++e->fresh; e->grabbed=1; }
}

/* ---------------- surface ---------------- */

EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig cfg, const EGLint *attribs) {
    typedef EGLSurface (*F)(EGLDisplay, EGLConfig, const EGLint *);
    F f = GLE("eglCreatePbufferSurface", F);
    if (!f) return EGL_NO_SURFACE;
    EGLSurface s = f(dpy, cfg, attribs);
    if (s == EGL_NO_SURFACE) { __android_log_print(ANDROID_LOG_WARN, "d3d12shim", "eglCreatePbufferSurface -> FAIL"); return s; }
    EGLint w = 0, h = 0;
    for (const EGLint *a = attribs; a && a[0] != EGL_NONE; a += 2) {
        if (a[0] == EGL_WIDTH) w = a[1];
        if (a[0] == EGL_HEIGHT) h = a[1];
    }
    surf_add(s, w, h, 0, NULL);
    return s;
}

EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig cfg, EGLNativeWindowType win, const EGLint *attribs) {
    if (shim_use_angle()) {
        typedef EGLSurface (*F)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *);
        F a = (F)angle_sym("eglCreateWindowSurface");
        EGLSurface s = a ? a(dpy, cfg, win, attribs) : EGL_NO_SURFACE;
        return s;
    }
    (void)attribs;
    probe_android();
    EGLint w = 640, h = 480;
    EGLint ww = -1, hh = -1;
    if (win && p_ANativeWindow_getWidth) {
        ww = p_ANativeWindow_getWidth((ANativeWindow *)win);
        hh = p_ANativeWindow_getHeight((ANativeWindow *)win);
        if (ww > 0) w = ww;
        if (hh > 0) h = hh;
    }

    /* 第 60 轮：同一个 ANativeWindow 反复 eglCreateWindowSurface 时复用已有 surface。
     * 实测 launcher 的主窗口（1920x1048）在 boot 期被创建了 15 次 —— 窗口每 relayout
     * 一次就一个新 surface，每次都新 pbuffer。而 app 那个 context 的 default
     * framebuffer 只跟着**第一次**的 surface：之后 app 的 draw 落进新 pbuffer，
     * present 却读不到（nz=0、err=0），换成新 context 绑同一块 surface 又能读到内容。
     * 复用能保住 context 的 default fb 和 surface 的绑定关系，也让 15 次重建变 1 次。
     * 只复用**同一个 win 指针 + 同尺寸**的；尺寸变了说明窗口真的改了，照旧新建。 */
    int owns_connection = win && dpy &&
        dpy == __atomic_load_n(&android_angle_display, __ATOMIC_ACQUIRE);
    if (win && !owns_connection) {
        for (int i = 0; i < g_nsurf; i++) {
            if (g_surf[i].egl_surf && g_surf[i].win == win && g_surf[i].is_window &&
                g_surf[i].w == w && g_surf[i].h == h) {
                return g_surf[i].egl_surf;
            }
        }
    }
    typedef EGLSurface (*F)(EGLDisplay, EGLConfig, const EGLint *);
    F f = GLE("eglCreatePbufferSurface", F);
    if (!f) {
        __android_log_print(ANDROID_LOG_WARN, "d3d12shim", "WINSURF FAIL: eglCreatePbufferSurface 符号缺失 win=%p", (void *)win);
        return EGL_NO_SURFACE;
    }
    const EGLint pattr[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
    EGLSurface s = f(dpy, cfg, pattr);
    if (s == EGL_NO_SURFACE) return s;
    {   shim_surf_t *se = surf_add(s, w, h, 1, (ANativeWindow *)win);
        if (owns_connection) {
            EGLint format, color = EGL_GL_COLORSPACE_LINEAR_KHR;
            for (const EGLint *a = attribs; a && a[0] != EGL_NONE; a += 2)
                if (a[0] == EGL_GL_COLORSPACE_KHR) color = a[1];
            if (!se || !eglGetConfigAttrib(dpy, cfg, EGL_NATIVE_VISUAL_ID, &format) ||
                shim_connect_window((ANativeWindow *)win, format, color)) {
                if (se) surf_del(s);
                typedef EGLBoolean (*Destroy)(EGLDisplay, EGLSurface);
                Destroy destroy = GLE("eglDestroySurface", Destroy);
                if (destroy) destroy(dpy, s);
                window_error = EGL_BAD_ALLOC;
                return EGL_NO_SURFACE;
            }
            se->owns_window_connection = 1;
        }
        if (se) { se->dpy = dpy; se->cfg = cfg; }
        else {
            __android_log_print(ANDROID_LOG_WARN, "d3d12shim", "WINSURF surf_add 失败（表满）surf=%p nsurf=%d", (void *)s, g_nsurf);
        }
    }
    return s;
}

EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surf) {
    {   shim_surf_t *e = surf_find(surf);
        if (e && e->ab) { ab_view_destroy(e->ab); e->ab = NULL; }
        if (e && e->owns_window_connection) shim_disconnect_window(e->win);
    }
    surf_del(surf);
    typedef EGLBoolean (*F)(EGLDisplay, EGLSurface);
    F f = GLE("eglDestroySurface", F);
    return f ? f(dpy, surf) : EGL_FALSE;
}

EGLBoolean eglQuerySurface(EGLDisplay dpy, EGLSurface surf, EGLint attr, EGLint *val) {
    shim_surf_t *e = surf_find(surf);
    if (e) {
        if (attr == EGL_WIDTH) { *val = e->w; return EGL_TRUE; }
        if (attr == EGL_HEIGHT) { *val = e->h; return EGL_TRUE; }
    }
    typedef EGLBoolean (*F)(EGLDisplay, EGLSurface, EGLint, EGLint *);
    F f = GLE("eglQuerySurface", F);
    return f ? f(dpy, surf, attr, val) : EGL_FALSE;
}

EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    if (shim_use_angle()) {
        typedef EGLBoolean (*F)(EGLDisplay, EGLSurface);
        F a = (F)angle_sym("eglSwapBuffers");
        return a ? a(dpy, surf) : EGL_FALSE;
    }
    /* SF/Skia 可能把一帧渲染进 gralloc 的 EGLImage FBO，swap 前统一回写 */
    egl_img_flush_all();
    shim_surf_t *e = surf_find(surf);
    if (e) return present(dpy, e);
    typedef EGLBoolean (*F)(EGLDisplay, EGLSurface);
    F f = GLE("eglSwapBuffers", F);
    EGLBoolean r = f ? f(dpy, surf) : EGL_TRUE;
    return r;
}

EGLBoolean eglSwapInterval(EGLDisplay dpy, EGLint interval) {
    typedef EGLBoolean (*F)(EGLDisplay, EGLint);
    F f = GLE("eglSwapInterval", F);
    return f ? f(dpy, interval) : EGL_TRUE;
}

/* ---------------- proc ---------------- */

/* 前置声明：定义在文件后面，eglGetProcAddress 需要引用 */
EGLImageKHR eglCreateImageKHR(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
EGLImage eglCreateImage(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLAttrib *);
EGLBoolean eglDestroyImageKHR(EGLDisplay, EGLImageKHR);
EGLBoolean eglDestroyImage(EGLDisplay, EGLImage);
uintptr_t shim_fwd_eglGetNativeClientBufferANDROID(uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                                                   uintptr_t, uintptr_t, uintptr_t, uintptr_t);

/* 我们自己的 glEGLImageTargetTexture2DOES：Mesa 拿到的是假 image，不能给它 */
static void shim_image_target_texture2d(unsigned target, void *img) {
    int ours = egl_img_target_texture(target, img);
    if (ours) return;
    typedef void (*F)(unsigned, void *);
    static F f;
    if (!f) f = (F)GLP("glEGLImageTargetTexture2DOES");
    if (f) f(target, img);
}

__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *name) {
    if (ab_is_busy()) {          /* ANGLE 初始化中：还它自己的指针，别给 Mesa 的 */
        void *p = ab_angle_gles_proc(name);
        if (p) return (__eglMustCastToProperFunctionPointerType)p;
        return NULL;
    }
    if (shim_use_angle()) {
        /* 注意：不能直接调 ANGLE 的 eglGetProcAddress —— meta-EGL 在
         * Loader::open 里填 dispatch 表时就会调我们，那时 ANGLE 的 EGL 还没
         * eglInitialize 过。直接从 ANGLE 的 GLES 库里 dlsym 最稳。 */
        if (name) {
            if (g_angle_gles2) {
                void *p = dlsym(g_angle_gles2, name);
                if (p) return (__eglMustCastToProperFunctionPointerType)p;
            }
            if (g_angle_gles1) {
                void *p = dlsym(g_angle_gles1, name);
                if (p) return (__eglMustCastToProperFunctionPointerType)p;
            }
            if (!strncmp(name, "egl", 3)) {
                void *p = angle_sym(name);
                if (p) return (__eglMustCastToProperFunctionPointerType)p;
            }
        }
        return NULL;
    }
    if (name && !strcmp(name, "eglTerminate"))
        return (__eglMustCastToProperFunctionPointerType)eglTerminate;
    if (name && !strcmp(name, "eglDestroyContext"))
        return (__eglMustCastToProperFunctionPointerType)eglDestroyContext;
    if (name && !strcmp(name, "glEGLImageTargetTexture2DOES"))
        return (__eglMustCastToProperFunctionPointerType)shim_image_target_texture2d;
    /* Skia/SurfaceFlinger 的 RenderEngine 通过 eglGetProcAddress 拿这些扩展函数：
     * 必须还我们自己的实现，否则会拿到 Mesa 的（不支持 Android native buffer）。 */
    if (name && !strcmp(name, "eglCreateImageKHR"))
        return (__eglMustCastToProperFunctionPointerType)eglCreateImageKHR;
    if (name && !strcmp(name, "eglCreateImage"))
        return (__eglMustCastToProperFunctionPointerType)eglCreateImage;
    if (name && !strcmp(name, "eglDestroyImageKHR"))
        return (__eglMustCastToProperFunctionPointerType)eglDestroyImageKHR;
    if (name && !strcmp(name, "eglDestroyImage"))
        return (__eglMustCastToProperFunctionPointerType)eglDestroyImage;
    if (name && !strcmp(name, "eglGetNativeClientBufferANDROID"))
        return (__eglMustCastToProperFunctionPointerType)shim_fwd_eglGetNativeClientBufferANDROID;
    /* ---- 第 59 轮（P0 根因）：EGL 核心入口也不能漏 ----
     * 这里以前只白名单了几个 image 相关函数，其余全部落到 Mesa 的 eglGetProcAddress。
     * 于是 app 用 eglGetProcAddress("eglSwapBuffers") 拿到的**是 Mesa 的 swap**，
     * 根本不进我们的 present —— 帧画得再好也不会写进 ANativeWindow。
     *
     * 证据：launcher 的 SF 层是 visible、有 buffer、frame=17，gfxinfo 报 43 帧，
     * 而我们自己的 eglSwapBuffers 计数只有 1；settings 的计数 61，所以它一直正常。
     *
     * 这几个函数回来之后，swap 才会重新走 present。 */
    if (name && (!strcmp(name, "eglSwapBuffers") ||
                 !strcmp(name, "eglSwapBuffersWithDamageKHR")))
        return (__eglMustCastToProperFunctionPointerType)eglSwapBuffers;
    if (name && !strcmp(name, "eglMakeCurrent"))
        return (__eglMustCastToProperFunctionPointerType)eglMakeCurrent;
    if (name && !strcmp(name, "eglGetCurrentSurface"))
        return (__eglMustCastToProperFunctionPointerType)eglGetCurrentSurface;
    if (name && !strcmp(name, "eglGetCurrentContext"))
        return (__eglMustCastToProperFunctionPointerType)eglGetCurrentContext;
    if (name && !strcmp(name, "eglGetCurrentDisplay"))
        return (__eglMustCastToProperFunctionPointerType)eglGetCurrentDisplay;
    typedef __eglMustCastToProperFunctionPointerType (*F)(const char *);
    F f = GLE("eglGetProcAddress", F);
    void *p = f ? (void *)f(name) : NULL;
    if (!p) p = P(name);
    return (__eglMustCastToProperFunctionPointerType)p;
}

/* ---------------- EGLImage（Android native buffer，CPU 中转） ---------------- */

EGLImageKHR eglCreateImageKHR(EGLDisplay dpy, EGLContext ctx, EGLenum target,
                              EGLClientBuffer buffer, const EGLint *attribs) {
    if (shim_use_angle()) {
        typedef EGLImageKHR (*F)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
        F a = (F)angle_sym("eglCreateImageKHR");
        return a ? a(dpy, ctx, target, buffer, attribs) : EGL_NO_IMAGE_KHR;
    }
    void *im = egl_img_create((unsigned)target, (void *)buffer, attribs);
    if (im) return (EGLImageKHR)im;
    typedef EGLImageKHR (*F)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
    F f = (F)GLP("eglCreateImageKHR");
    EGLImageKHR r = f ? f(dpy, ctx, target, buffer, attribs) : EGL_NO_IMAGE_KHR;
    return r;
}

EGLBoolean eglDestroyImageKHR(EGLDisplay dpy, EGLImageKHR image) {
    if (egl_img_destroy((void *)image)) return EGL_TRUE;
    typedef EGLBoolean (*F)(EGLDisplay, EGLImageKHR);
    F f = (F)GLP("eglDestroyImageKHR");
    return f ? f(dpy, image) : EGL_TRUE;
}

EGLImage eglCreateImage(EGLDisplay dpy, EGLContext ctx, EGLenum target,
                        EGLClientBuffer buffer, const EGLAttrib *attribs) {
    if (shim_use_angle()) {
        typedef EGLImage (*F)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLAttrib *);
        F a = (F)angle_sym("eglCreateImage");
        return a ? a(dpy, ctx, target, buffer, attribs) : (EGLImage)0;
    }
    void *im = egl_img_create((unsigned)target, (void *)buffer, attribs);
    if (im) return (EGLImage)im;
    typedef EGLImage (*F)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLAttrib *);
    F f = GLE("eglCreateImage", F);
    return f ? f(dpy, ctx, target, buffer, attribs) : (EGLImage)0;
}

EGLBoolean eglDestroyImage(EGLDisplay dpy, EGLImage image) {
    if (egl_img_destroy((void *)image)) return EGL_TRUE;
    typedef EGLBoolean (*F)(EGLDisplay, EGLImage);
    F f = GLE("eglDestroyImage", F);
    return f ? f(dpy, image) : EGL_TRUE;
}

/* ---------------- 平台 window surface / damage 入口 ---------------- */

EGLSurface eglCreatePlatformWindowSurface(EGLDisplay dpy, EGLConfig cfg, void *native_window,
                                          const EGLAttrib *attribs) {
    if (shim_use_angle()) {
        typedef EGLSurface (*F)(EGLDisplay, EGLConfig, void *, const EGLAttrib *);
        F a = (F)angle_sym("eglCreatePlatformWindowSurface");
        return a ? a(dpy, cfg, native_window, attribs) : EGL_NO_SURFACE;
    }
    EGLint color[] = { EGL_GL_COLORSPACE_KHR, EGL_GL_COLORSPACE_LINEAR_KHR, EGL_NONE };
    for (const EGLAttrib *a = attribs; a && a[0] != EGL_NONE; a += 2)
        if (a[0] == EGL_GL_COLORSPACE_KHR) color[1] = (EGLint)a[1];
    return eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)native_window, color);
}

EGLSurface eglCreatePlatformWindowSurfaceEXT(EGLDisplay dpy, EGLConfig cfg, void *native_window,
                                             const EGLint *attribs) {
    if (shim_use_angle()) {
        typedef EGLSurface (*F)(EGLDisplay, EGLConfig, void *, const EGLint *);
        F a = (F)angle_sym("eglCreatePlatformWindowSurfaceEXT");
        return a ? a(dpy, cfg, native_window, attribs) : EGL_NO_SURFACE;
    }
    return eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)native_window, attribs);
}

EGLBoolean eglSwapBuffersWithDamageKHR(EGLDisplay dpy, EGLSurface surf,
                                       const EGLint *rects, EGLint n_rects) {
    if (shim_use_angle()) {
        typedef EGLBoolean (*F)(EGLDisplay, EGLSurface, const EGLint *, EGLint);
        F a = (F)angle_sym("eglSwapBuffersWithDamageKHR");
        if (a) return a(dpy, surf, rects, n_rects);
    }
    /* Damage describes the window, not every image in the context. Flush once in swap. */
    (void)rects; (void)n_rects;
    return eglSwapBuffers(dpy, surf);
}

EGLBoolean eglSetDamageRegionKHR(EGLDisplay dpy, EGLSurface surf,
                                 const EGLint *rects, EGLint n_rects) {
    (void)dpy; (void)surf; (void)rects; (void)n_rects;
    return EGL_TRUE;
}

/* =====================================================================
 * Android 的 libEGL.so 是一个 loader：它按 ro.hardware.egl 去 dlopen
 * /vendor/lib64/egl/libEGL_<tag>.so，然后对一批 EGL 入口逐个 dlsym。
 * 少一个就可能整条驱动被判定不可用，所以下面把其余入口都补成薄转发。
 *
 * 转发用的统一签名是两个指针参数不够用的——直接用 8 个寄存器参数，
 * SysV x86-64 上前 6 个走寄存器、其余走栈，和调用方实际压的完全一致；
 * EGL 的参数只有整数和指针（没有浮点），所以这样转发是安全的。
 * 返回值统一按 uintptr_t 取，EGLBoolean(32 位) 的调用方只读 eax。
 *
 * 注意：这些函数名和 egl.h 里的原型冲突，所以 C 侧用 shim_fwd_<name>，
 * 再用 .set 在汇编层做同名别名导出。
 * ===================================================================== */

#define SHIM_FWD8(name) \
    __attribute__((visibility("default"))) \
    uintptr_t shim_fwd_##name(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, \
                              uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7) { \
        typedef uintptr_t (*F)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, \
                               uintptr_t, uintptr_t, uintptr_t, uintptr_t); \
        static F f; \
        if (!f) f = (F)P(#name); \
        if (!f) return 0; \
        return f(a0, a1, a2, a3, a4, a5, a6, a7); \
    } \
    __asm__(".globl " #name "\n.set " #name ", shim_fwd_" #name "\n");

/* Mesa 没有的（Android/ANGLE 扩展、stream 之类），返回失败但不崩 */
#define SHIM_STUB8(name, ret) \
    __attribute__((visibility("default"))) \
    uintptr_t shim_fwd_##name(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, \
                              uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7) { \
        (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; \
        return (ret); \
    } \
    __asm__(".globl " #name "\n.set " #name ", shim_fwd_" #name "\n");

/* 下面这些在 Mesa 的 libEGL 里都存在，纯转发即可 */
SHIM_FWD8(eglBindTexImage)
SHIM_FWD8(eglClientWaitSync)
EGLint eglClientWaitSyncKHR(EGLDisplay dpy, EGLSyncKHR sync, EGLint flags, EGLTimeKHR timeout) {
    PFNEGLCLIENTWAITSYNCKHRPROC fn = (PFNEGLCLIENTWAITSYNCKHRPROC)GLP("eglClientWaitSyncKHR");
    return fn ? fn(dpy, sync, flags, timeout) : EGL_FALSE;
}
SHIM_FWD8(eglCopyBuffers)
SHIM_FWD8(eglCreatePbufferFromClientBuffer)
SHIM_FWD8(eglCreatePixmapSurface)
SHIM_FWD8(eglCreatePlatformPixmapSurface)
SHIM_FWD8(eglCreatePlatformPixmapSurfaceEXT)
SHIM_FWD8(eglCreateSync)
EGLSyncKHR eglCreateSyncKHR(EGLDisplay dpy, EGLenum type, const EGLint *attribs) {
    PFNEGLCREATESYNCKHRPROC fn = (PFNEGLCREATESYNCKHRPROC)GLP("eglCreateSyncKHR");
    return fn ? fn(dpy, type, attribs) : EGL_NO_SYNC_KHR;
}
SHIM_FWD8(eglDestroySync)
EGLBoolean eglDestroySyncKHR(EGLDisplay dpy, EGLSyncKHR sync) {
    PFNEGLDESTROYSYNCKHRPROC fn = (PFNEGLDESTROYSYNCKHRPROC)GLP("eglDestroySyncKHR");
    return fn ? fn(dpy, sync) : EGL_FALSE;
}
/* eglGetConfigAttrib / eglGetConfigs 必须用上面带逻辑的实现：
 * 这里的 .set 别名会把符号值改到 shim_fwd_ 版本，等于把真实的实现屏蔽掉。 */
SHIM_FWD8(eglGetCurrentContext)
SHIM_FWD8(eglGetCurrentDisplay)
SHIM_FWD8(eglGetCurrentSurface)
SHIM_FWD8(eglGetSyncAttrib)
EGLBoolean eglGetSyncAttribKHR(EGLDisplay dpy, EGLSyncKHR sync, EGLint attribute, EGLint *value) {
    PFNEGLGETSYNCATTRIBKHRPROC fn = (PFNEGLGETSYNCATTRIBKHRPROC)GLP("eglGetSyncAttribKHR");
    return fn ? fn(dpy, sync, attribute, value) : EGL_FALSE;
}
SHIM_FWD8(eglQueryAPI)
SHIM_FWD8(eglQueryContext)
SHIM_FWD8(eglReleaseTexImage)
SHIM_FWD8(eglReleaseThread)
SHIM_FWD8(eglSurfaceAttrib)
SHIM_FWD8(eglSwapInterval)
SHIM_FWD8(eglWaitClient)
SHIM_FWD8(eglWaitGL)
SHIM_FWD8(eglWaitNative)
SHIM_FWD8(eglWaitSync)
EGLint eglWaitSyncKHR(EGLDisplay dpy, EGLSyncKHR sync, EGLint flags) {
    PFNEGLWAITSYNCKHRPROC fn = (PFNEGLWAITSYNCKHRPROC)GLP("eglWaitSyncKHR");
    return fn ? fn(dpy, sync, flags) : EGL_FALSE;
}

SHIM_STUB8(eglCreateNativeClientBufferANDROID, 0)
SHIM_STUB8(eglCreateStreamFromFileDescriptorKHR, 0)
SHIM_STUB8(eglCreateStreamKHR, 0)
SHIM_STUB8(eglCreateStreamProducerSurfaceKHR, 0)
SHIM_STUB8(eglDestroyStreamKHR, 0)
SHIM_STUB8(eglDupNativeFenceFDANDROID, 0xffffffffu)
SHIM_STUB8(eglGetCompositorTimingANDROID, 0)
SHIM_STUB8(eglGetCompositorTimingSupportedANDROID, 0)
SHIM_STUB8(eglGetFrameTimestampSupportedANDROID, 0)
SHIM_STUB8(eglGetFrameTimestampsANDROID, 0)
/* eglGetNativeClientBufferANDROID: 原样把 AHardwareBuffer* 当 EGLClientBuffer 返回。
 * Skia/SurfaceFlinger 的 RenderEngine 走 eglGetNativeClientBufferANDROID -> 
 * eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID)，返回 0 会让整条路径断掉。 */
__attribute__((visibility("default")))
uintptr_t shim_fwd_eglGetNativeClientBufferANDROID(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3,
                                                   uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    return a0;
}
__asm__(".globl eglGetNativeClientBufferANDROID\n.set eglGetNativeClientBufferANDROID, shim_fwd_eglGetNativeClientBufferANDROID\n");
SHIM_STUB8(eglGetNextFrameIdANDROID, 0)
SHIM_STUB8(eglGetRenderBufferANDROID, 0)
SHIM_STUB8(eglGetStreamFileDescriptorKHR, 0)
SHIM_STUB8(eglGetSystemTimeFrequencyNV, 0)
SHIM_STUB8(eglGetSystemTimeNV, 0)
SHIM_STUB8(eglLockSurfaceKHR, 0)
SHIM_STUB8(eglPresentationTimeANDROID, 0)
SHIM_STUB8(eglQueryStreamKHR, 0)
SHIM_STUB8(eglQueryStreamTimeKHR, 0)
SHIM_STUB8(eglQueryStreamu64KHR, 0)
SHIM_STUB8(eglSetBlobCacheFuncsANDROID, 0)
SHIM_STUB8(eglSetSwapRectangleANDROID, 0)
SHIM_STUB8(eglSignalSyncKHR, 0)
SHIM_STUB8(eglStreamAttribKHR, 0)
SHIM_STUB8(eglStreamConsumerAcquireKHR, 0)
SHIM_STUB8(eglStreamConsumerGLTextureExternalKHR, 0)
SHIM_STUB8(eglStreamConsumerReleaseKHR, 0)
SHIM_STUB8(eglUnlockSurfaceKHR, 0)

/* 兼容旧测试 */
int egl_shim_init(void) { return glibc_ldr_init(); }
