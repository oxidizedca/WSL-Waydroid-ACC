/*
 * egl_present_angle.c —— 方案 B：present 桥（第 40 轮）
 *
 * 背景（第 37 轮结论）：HWUI 的 ReliableSurface 会先 native_window_api_connect(API_EGL)，
 * 我们的 present 用 ANativeWindow_lock()（内部 connect(API_CPU)），BufferQueueProducer
 * 对"已经连了别的 API"直接返回 -22，于是 app 的 buffer 一个都进不了 SF，画面永远白屏。
 *
 * 解法：app 仍然用我们的 D3D12/Mesa 渲进 pbuffer（这条路径是好的），
 * present 时把 readback 的 RGBA 上传成纹理，借原版 ANGLE 建一个真窗口 surface，
 * 画一个全屏 quad，再 eglSwapBuffers —— ANGLE 走 EGL 路径入队，不会有 API 冲突。
 *
 * 开关：/data/local/tmp/angle_present（不存在 = 完全走老路径）
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include <android/log.h>
#define BRIDGE_WARN(...) __android_log_print(ANDROID_LOG_WARN, "d3d12shim", __VA_ARGS__)

/* ---- 线程局部"正在 ANGLE 初始化"标记 ----
 * ANGLE 的 EGL 会按 soname 去加载它的 GLES 驱动（libGLESv2_angle.so），
 * 而那个名字现在是我们的转发库。所以 ANGLE 初始化期间，我们的 GL/EGL 入口
 * 必须把函数指针还给 ANGLE 自己（libGLESv2_angle_orig.so），否则 ANGLE 拿到
 * 的是 Mesa 的指针 → 它内部 tail-jump 到 NULL（第 40 轮 tombstone: rip=0）。
 * 用 __thread：只在做初始化的那条线程上生效，不影响 app 自己的渲染线程。 */
__thread int g_ab_busy = 0;
void ab_set_busy(int v) { g_ab_busy = v; }
int  ab_is_busy(void) { return g_ab_busy; }

typedef void *V;
typedef int32_t I;

/* ---------------- 懒加载 ---------------- */
static int g_state = -1;           /* -1 未判定 / 0 关 / 1 开 */
static void *g_egl_h, *g_gles_h;

static V (*e_GetDisplay)(V);
static I (*e_Initialize)(V, I *, I *);
static V (*e_CreateContext)(V, V, V, const I *);
static V (*e_CreateWindowSurface)(V, V, void *, const I *);
static I (*e_MakeCurrent)(V, V, V, V);
static I (*e_SwapBuffers)(V, V);
static I (*e_DestroySurface)(V, V);
static I (*e_DestroyContext)(V, V);
static I (*e_GetError)(void);
static I (*e_GetConfigs)(V, V *, I, I *);
static I (*e_GetConfigAttrib)(V, V, I, I *);
static V (*e_CreatePlatformWindowSurface)(V, V, void *, const int64_t *);
static I (*e_BindAPI)(unsigned);

typedef struct {
    void (*GenTextures)(I, unsigned *);
    void (*BindTexture)(unsigned, unsigned);
    void (*TexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
    void (*TexSubImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
    void (*TexParameteri)(unsigned, unsigned, int);
    void (*PixelStorei)(unsigned, int);
    void (*Viewport)(int, int, int, int);
    void (*ClearColor)(float, float, float, float);
    void (*Clear)(unsigned);
    unsigned (*CreateShader)(unsigned);
    void (*ShaderSource)(unsigned, int, const char *const *, const int *);
    void (*CompileShader)(unsigned);
    unsigned (*CreateProgram)(void);
    void (*AttachShader)(unsigned, unsigned);
    void (*LinkProgram)(unsigned);
    void (*UseProgram)(unsigned);
    int (*GetAttribLocation)(unsigned, const char *);
    int (*GetUniformLocation)(unsigned, const char *);
    void (*EnableVertexAttribArray)(unsigned);
    void (*VertexAttribPointer)(unsigned, int, unsigned, unsigned char, int, const void *);
    void (*DrawArrays)(unsigned, int, int);
} gl_t;
static gl_t G;

/* ANGLE 自己那份 GLES（初始化窗口内回给它用） */
void *ab_angle_gles_proc(const char *n) { return (g_gles_h && n) ? dlsym(g_gles_h, n) : NULL; }

#define LD(sym, name) do { *(void **)(&(sym)) = dlsym(g_gles_h, name); } while (0)

static int load(void) {
    if (g_state >= 0) return g_state;
    g_state = 0;
    if (access("/data/local/tmp/angle_present", R_OK) != 0) return 0;

    /* 第 43 轮：ANGLE 的 EGL 副本已经"私有改名"（rename_angle.py）——
     * 它的 DT_NEEDED 从 libGLESv2_angle.so 改成了 libgles2ang.so，指向它自己的
     * GLES 副本。这样 /system/lib64/libGLESv2_angle.so 可以继续装我们的转发库
     * （app 侧 meta-EGL 的 dispatch 表靠它），而 ANGLE 初始化时不会再抓到我们的库。 */
    g_egl_h = dlopen("libEGL_angle_orig.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_egl_h) g_egl_h = dlopen("/data/local/tmp/angle_orig/libEGL_angle_orig.so", RTLD_NOW | RTLD_LOCAL);
    g_gles_h = dlopen("libgles2ang.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_gles_h) g_gles_h = dlopen("/data/local/tmp/angle_orig/libgles2ang.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_egl_h || !g_gles_h) {
        printf("[bridge] dlopen ANGLE failed: egl=%p gles=%p (%s)\n", g_egl_h, g_gles_h, dlerror());
        return 0;
    }
#define LE(sym, name) do { *(void **)(&(sym)) = dlsym(g_egl_h, name); } while (0)
    LE(e_GetDisplay, "eglGetDisplay");
    LE(e_Initialize, "eglInitialize");
    LE(e_CreateContext, "eglCreateContext");
    LE(e_CreateWindowSurface, "eglCreateWindowSurface");
    LE(e_MakeCurrent, "eglMakeCurrent");
    LE(e_SwapBuffers, "eglSwapBuffers");
    LE(e_DestroySurface, "eglDestroySurface");
    LE(e_DestroyContext, "eglDestroyContext");
    LE(e_GetError, "eglGetError");
    LE(e_GetConfigs, "eglGetConfigs");
    LE(e_GetConfigAttrib, "eglGetConfigAttrib");
    LE(e_CreatePlatformWindowSurface, "eglCreatePlatformWindowSurface");
    LE(e_BindAPI, "eglBindAPI");

    LD(G.GenTextures, "glGenTextures");
    LD(G.BindTexture, "glBindTexture");
    LD(G.TexImage2D, "glTexImage2D");
    LD(G.TexSubImage2D, "glTexSubImage2D");
    LD(G.TexParameteri, "glTexParameteri");
    LD(G.PixelStorei, "glPixelStorei");
    LD(G.Viewport, "glViewport");
    LD(G.ClearColor, "glClearColor");
    LD(G.Clear, "glClear");
    LD(G.CreateShader, "glCreateShader");
    LD(G.ShaderSource, "glShaderSource");
    LD(G.CompileShader, "glCompileShader");
    LD(G.CreateProgram, "glCreateProgram");
    LD(G.AttachShader, "glAttachShader");
    LD(G.LinkProgram, "glLinkProgram");
    LD(G.UseProgram, "glUseProgram");
    LD(G.GetAttribLocation, "glGetAttribLocation");
    LD(G.GetUniformLocation, "glGetUniformLocation");
    LD(G.EnableVertexAttribArray, "glEnableVertexAttribArray");
    LD(G.VertexAttribPointer, "glVertexAttribPointer");
    LD(G.DrawArrays, "glDrawArrays");
#undef LE
#undef LD
    if (!e_GetDisplay || !e_Initialize || !e_GetConfigs || !e_CreateContext ||
        !e_CreateWindowSurface || !e_MakeCurrent || !e_SwapBuffers ||
        !G.GenTextures || !G.DrawArrays || !G.CreateProgram) {
        printf("[bridge] ANGLE 入口不全，禁用\n");
        return 0;
    }
    g_state = 1;
    return 1;
}

int ab_enabled(void) { return load(); }

/* ---------------- libandroid 句柄（懒加载一次） ---------------- */
static void *g_and_h;
static int32_t (*g_winF)(void *);

static void ab_and_load(void) {
    if (g_and_h) return;
    g_and_h = dlopen("libandroid.so", RTLD_NOW);
    if (!g_and_h) { BRIDGE_WARN("bridge: dlopen libandroid.so 失败: %s", dlerror()); return; }
    *(void **)(&g_winF) = dlsym(g_and_h, "ANativeWindow_getFormat");
}


typedef int (*ab_q_fn)(void *, int, int *);
typedef int (*ab_p_fn)(void *, int, ...);

/* 直接走窗口自己的 query(2 = NATIVE_WINDOW_FORMAT)，比 libandroid 的 getFormat 更可靠
 * （实测后者在部分窗口上会返回垃圾） */
static int ab_win_format(void *window) {
    ab_q_fn pq = *(ab_q_fn *)((char *)window + 0x90);
    int v = -1;
    if (pq) { int r = pq(window, 2, &v); if (r != 0) v = -100 - r; }
    else { ab_and_load(); v = g_winF ? g_winF(window) : -1; }
    return v;
}

/* ---- 第 47 轮：窗口函数指针钩子（真因与解法）----
 *
 * 真因链：
 *   1. HWUI 的 ReliableSurface 在窗口创建时就 native_window_api_connect(API_EGL) 并一直占着；
 *   2. BufferQueueProducer::connect 对"任何"重复连接都返回 -EINVAL（同一个 API 也不例外），
 *      所以我们的 ANativeWindow_lock(API_CPU) 和 ANGLE 的 perform(API_CONNECT, API_EGL) 都是 -22；
 *   3. ANGLE 的 eglCreateWindowSurface 第一步就是 connect，拿到 -22 → 它只能返回 EGL_BAD_CONFIG。
 *
 * 解法：临时把窗口的 query/perform 换成钩子，ANGLE 调 API_CONNECT 时先 disconnect 再 connect，
 *      建完 surface 立刻把原指针装回去。窗口其它成员（dequeue/queue…）保持原样，this 也是真窗口，
 *      所以不存在代理对象的 this 问题。
 *
 * 布局来自 libandroid.so 反汇编：query @0x90，perform @0x98。 */
static ab_q_fn g_orig_q;
static ab_p_fn g_orig_p;   /* 汇编钩子按名字直接取（static 才不会有 PIC 重定位问题） */
static void *g_hooked_win;

/* 当前窗口的属性，供 ab_hook_query 自己回答用 */
static int g_win_w = 1080, g_win_h = 1920, g_win_fmt = 1;


/* 第 47 轮：完全不转发给真窗口。
 * 原因：真窗口的 Surface::query 里有内部路径会踩空（query(21) → getConsumerUsage →
 * mov %rax,(%rbx) 且 rbx 不可用，崩在 libgui+0x117328）。ANGLE 建窗口 surface 时问的
 * 就是那几个固定 what，我们自己回答，行为可控且绝对不会把进程带崩。 */
static int ab_hook_query(void *w, int what, int *out) {
    (void)w;
    int v = 0;
    switch (what) {
        case 0:  case 6:  v = g_win_w; break;   /* WIDTH / DEFAULT_WIDTH */
        case 1:  case 7:  v = g_win_h; break;   /* HEIGHT / DEFAULT_HEIGHT */
        case 2:           v = g_win_fmt; break; /* FORMAT */
        case 3:           v = 1; break;         /* MIN_UNDEQUEUED_BUFFERS */
        case 8:           v = 0; break;         /* TRANSFORM_HINT */
        case 9:           v = 1; break;         /* BUFFER_AGE */
        case 21:          v = 0; break;         /* CONSUMER_USAGE_BITS */
        default:          v = 0; break;
    }
    if (out) *out = v;
    return 0;
}

/* 只在 API_CONNECT 时介入；其余 op 一个参数都不动地 tail-jump 给真函数。
 * 用 C 的 varargs 转发会把 int64/指针参数截断（实测会把 HWUI 的调用带崩），所以这里必须是汇编。 */
__attribute__((used)) int ab_do_connect(void *w, int op, int api) {
    (void)op;
    if (g_orig_p) g_orig_p(w, 5 /*NATIVE_WINDOW_API_DISCONNECT*/, api);
    int r = g_orig_p ? g_orig_p(w, 4 /*NATIVE_WINDOW_API_CONNECT*/, api) : -1;
    return r;
}

/* 窗口另外两个会被 ANGLE 调用的成员（偏移 0x28 / 0x70）。
 * 假窗口实验里它们返回 0 就能让 ANGLE 顺利建出 surface；真窗口上它们会去碰
 * BufferQueue（未连接 → 失败），所以这里先照假窗口的行为直接回 0。 */
__attribute__((used)) int ab_stub28(void *w, ...) {
    (void)w;
    return 0;
}
__attribute__((used)) int ab_stub70(void *w, ...) {
    (void)w;
    return 0;
}

__attribute__((naked)) static int ab_hook_perform(void) {
    __asm__ __volatile__(
        "cmpl $4, %esi\n\t"           /* op == NATIVE_WINDOW_API_CONNECT ? */
        "jne 1f\n\t"
        "jmp ab_do_connect\n\t"       /* rdi=win, esi=4, edx=api 原样传过去 */
        "1:\n\t"
        "leaq g_orig_p(%rip), %rax\n\t"
        "movq (%rax), %rax\n\t"
        "jmp *%rax\n\t"
    );
}

static void ab_hook_install(void *win, int w, int h, int fmt) {
    if (g_hooked_win) return;
    g_win_w = w > 0 ? w : 1080;
    g_win_h = h > 0 ? h : 1920;
    g_win_fmt = fmt > 0 ? fmt : 1;
    ab_q_fn *qp = (ab_q_fn *)((char *)win + 0x90);
    ab_p_fn *pp = (ab_p_fn *)((char *)win + 0x98);
    g_orig_q = *qp;
    g_orig_p = *pp;
    if (!g_orig_q || !g_orig_p) {
        BRIDGE_WARN("bridge: 窗口钩子跳过（q=%p p=%p）", (void *)g_orig_q, (void *)g_orig_p);
        return;
    }
    /* 第 51 轮：二分开关。默认两个都钩；放下面这些文件可以只要一个。 */
    int do_q = 1, do_p = 1;
    if (access("/data/local/tmp/hook_none", F_OK) == 0) { do_q = 0; do_p = 0; }
    else if (access("/data/local/tmp/hook_q_only", F_OK) == 0) { do_p = 0; }
    else if (access("/data/local/tmp/hook_p_only", F_OK) == 0) { do_q = 0; }
    if (do_q) *qp = ab_hook_query;
    if (do_p) *pp = (ab_p_fn)(void *)ab_hook_perform;
    /* 抢在 ANGLE 之前把这个窗口连上：它建 surface 的过程中会调窗口的某个成员
     * （偏移 0x28，实测 72 次），那个成员需要 BufferQueue 已经 connect。
     * HWUI 占着连接（-22），所以先 disconnect 再 connect。 */
    /* 注意：perform 的 op 是 NATIVE_WINDOW_*：
     *   1=CONNECT  2=DISCONNECT  4=SET_BUFFER_COUNT  5=SET_GEOMETRY
     * 之前这里写的是 5/4（注释却说 disconnect/connect），等于拿 SET_GEOMETRY
     * 和 SET_BUFFER_COUNT(1) 去打窗口，很容易把 BufferQueueCore 写坏。
     * 用 hook_none 时整段跳过。 */
    if (g_orig_p && (do_q || do_p)) {
        g_orig_p(win, 2 /*NATIVE_WINDOW_DISCONNECT*/, 1 /*API_CPU*/);
        g_orig_p(win, 1 /*NATIVE_WINDOW_CONNECT*/, 1 /*API_CPU*/);
    }
    /* 第 51 轮：不再替换窗口的 0x28/0x70 成员。
     * 实测那两个 stub 会让 HWUI 的 dequeueBuffer 拿到 format=777211904 这种垃圾参数
     * （堆被踩）。既然 patch 之后重复 connect 已经放行、预连接也成功，
     * ANGLE 调 0x28 时走真窗口的实现即可。 */
    g_hooked_win = win;
}

static void ab_hook_remove(void) {
    if (!g_hooked_win) return;
    *(ab_q_fn *)((char *)g_hooked_win + 0x90) = g_orig_q;
    *(ab_p_fn *)((char *)g_hooked_win + 0x98) = g_orig_p;
    g_hooked_win = 0;
}


/* ---------------- 每个窗口一份状态 ---------------- */
typedef struct {
    V dpy, cfg, ctx, surf;
    int w, h;
    unsigned tex, prog;
    int a_pos, a_uv, u_tex;
    int ready;
    int tex_inited;   /* 第一次必须先 TexImage2D 分配，否则 TexSubImage2D 是未定义纹理 */
} ab_view;

static const char *VS =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
static const char *FS =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "void main() { gl_FragColor = texture2D(u_tex, v_uv); }\n";

/* 全屏 quad（triangle strip）。readback 是 bottom-up，GL 的 viewport 也是 bottom-up，
 * 所以 uv 直接对齐即可，不用翻。 */
static const float QUAD[] = {
    -1.0f, -1.0f, 0.0f, 0.0f,
     1.0f, -1.0f, 1.0f, 0.0f,
    -1.0f,  1.0f, 0.0f, 1.0f,
     1.0f,  1.0f, 1.0f, 1.0f,
};

void *ab_view_create(void *window, int w, int h) {
    if (!load() || !window) return NULL;
    ab_view *v = (ab_view *)calloc(1, sizeof(ab_view));
    if (!v) { BRIDGE_WARN("bridge: calloc 失败"); return NULL; }
    v->w = w; v->h = h;

    /* 整个创建过程都置 busy：ANGLE 内部任何一次取 GL/EGL 指针都必须是它自己的 */
    ab_set_busy(1);

    v->dpy = e_GetDisplay((V)0);
    if (!v->dpy) {
        BRIDGE_WARN("bridge: eglGetDisplay(ANGLE) -> null err=0x%x",
                       e_GetError ? (unsigned)e_GetError() : 0u);
        goto fail;
    }
    {
        I ok = e_Initialize(v->dpy, NULL, NULL);
        if (!ok) goto fail;
    }
    if (e_BindAPI) {
        e_BindAPI(0x30A0 /*EGL_OPENGL_ES_API*/);
    }

    /* 第 45 轮：不再"挑"config —— 直接对每个支持 WINDOW+ES2 的 config 真建一次
     * 窗口 surface，建得出来的那个才用（之前挑第一个，回来 EGL_BAD_CONFIG(0x300D)）。
     * 第 47 轮：连 vis 过滤也去掉（穷举），并且把每个 config 的细节全打出来。 */
    {
        V cfgs[128];
        I ncfg = 0;
        int made = 0;
        int winfmt = ab_win_format(window);

        /* 建 ANGLE surface 之前挂窗口钩子：query 自己答、API_CONNECT 时先 disconnect 再 connect */
        ab_hook_install(window, w, h, winfmt);

        if (!e_GetConfigs || !e_GetConfigs(v->dpy, cfgs, 128, &ncfg)) {
            BRIDGE_WARN("bridge: eglGetConfigs 失败 err=0x%x",
                           e_GetError ? (unsigned)e_GetError() : 0u);
        } else {
            /* ② 穷举：每个候选 config 都真建一次窗口（两种入口），错误码全打 */
            for (I i = 0; i < ncfg && !made; i++) {
                I st = 0, rt = 0;
                if (e_GetConfigAttrib) {
                    e_GetConfigAttrib(v->dpy, cfgs[i], 0x3033, &st);
                    e_GetConfigAttrib(v->dpy, cfgs[i], 0x3040, &rt);
                }
                if (!(st & 0x0004) || !(rt & 0x0004)) continue;
                V s = e_CreateWindowSurface(v->dpy, cfgs[i], window, NULL);
                if (!s && e_CreatePlatformWindowSurface) {
                    s = e_CreatePlatformWindowSurface(v->dpy, cfgs[i], window, NULL);
                }
                if (s) {
                    v->cfg = cfgs[i];
                    v->surf = s;
                    made = 1;
                }
            }
        }

        ab_hook_remove();

        if (!made) {
            BRIDGE_WARN("bridge: 没有能建窗口 surface 的 ANGLE config（共 %d 个）win=%p %dx%d",
                           (int)ncfg, window, w, h);
            goto fail;
        }
    }

    {
        const I ctxattr[] = { 0x3098, 2, 0x3038 };   /* CONTEXT_CLIENT_VERSION=2 */
        v->ctx = e_CreateContext(v->dpy, v->cfg, (V)0, ctxattr);
        if (!v->ctx) {
            BRIDGE_WARN("bridge: eglCreateContext(ANGLE) 失败 err=0x%x",
                           e_GetError ? (unsigned)e_GetError() : 0u);
            goto fail;
        }
    }
    if (!e_MakeCurrent(v->dpy, v->surf, v->surf, v->ctx)) {
        BRIDGE_WARN("bridge: eglMakeCurrent(ANGLE) 失败 err=0x%x",
                       e_GetError ? (unsigned)e_GetError() : 0u);
        goto fail;
    }
    G.GenTextures(1, &v->tex);
    G.BindTexture(0x0DE1, v->tex);
    G.TexParameteri(0x0DE1, 0x2801, 0x2601);   /* MIN_FILTER=LINEAR */
    G.TexParameteri(0x0DE1, 0x2800, 0x2601);   /* MAG_FILTER=LINEAR */
    G.TexParameteri(0x0DE1, 0x2802, 0x812F);   /* WRAP_S=CLAMP_TO_EDGE */
    G.TexParameteri(0x0DE1, 0x2803, 0x812F);   /* WRAP_T=CLAMP_TO_EDGE */
    G.PixelStorei(0x0D05, 4);                  /* PACK/PACK_ALIGNMENT=4 */

    {
        unsigned vs = G.CreateShader(0x8B31), fs = G.CreateShader(0x8B30);
        G.ShaderSource(vs, 1, &VS, NULL);
        G.ShaderSource(fs, 1, &FS, NULL);
        G.CompileShader(vs);
        G.CompileShader(fs);
        v->prog = G.CreateProgram();
        G.AttachShader(v->prog, vs);
        G.AttachShader(v->prog, fs);
        G.LinkProgram(v->prog);
        G.UseProgram(v->prog);
        v->a_pos = G.GetAttribLocation(v->prog, "a_pos");
        v->a_uv = G.GetAttribLocation(v->prog, "a_uv");
        v->u_tex = G.GetUniformLocation(v->prog, "u_tex");
    }

    /* 交出当前 context，调用方负责把自己的 make current 回来 */
    e_MakeCurrent(v->dpy, (V)0, (V)0, (V)0);
    ab_set_busy(0);
    v->ready = 1;
    return v;

fail:
    if (v->dpy && e_MakeCurrent) e_MakeCurrent(v->dpy, (V)0, (V)0, (V)0);
    if (v->surf && e_DestroySurface) e_DestroySurface(v->dpy, v->surf);
    if (v->ctx && e_DestroyContext) e_DestroyContext(v->dpy, v->ctx);
    ab_set_busy(0);
    free(v);
    return NULL;
}

int ab_view_frame(void *st, const unsigned char *rgba, int w, int h) {
    ab_view *v = (ab_view *)st;
    static int fails = 0;
    if (!v || !v->ready) return 0;
    ab_set_busy(1);
    if (!e_MakeCurrent(v->dpy, v->surf, v->surf, v->ctx)) {
        if (fails++ < 3) BRIDGE_WARN("bridge: frame makeCurrent 失败 err=0x%x",
                                        e_GetError ? (unsigned)e_GetError() : 0u);
        ab_set_busy(0);
        return 0;
    }
    G.Viewport(0, 0, v->w, v->h);
    G.BindTexture(0x0DE1, v->tex);
    if (!v->tex_inited || w != v->w || h != v->h) {
        /* 第 51 轮：第一次必须 TexImage2D 真正分配存储；之前一直走 TexSubImage2D，
         * 于是纹理从未被定义，画出来就是空的。 */
        G.TexImage2D(0x0DE1, 0, 0x1908 /*RGBA*/, w, h, 0, 0x1908, 0x1401 /*UNSIGNED_BYTE*/, rgba);
        v->tex_inited = 1;
        v->w = w; v->h = h;
    } else {
        G.TexSubImage2D(0x0DE1, 0, 0, 0, w, h, 0x1908, 0x1401, rgba);
    }
    G.UseProgram(v->prog);
    if (v->a_pos >= 0) {
        G.EnableVertexAttribArray((unsigned)v->a_pos);
        G.VertexAttribPointer((unsigned)v->a_pos, 2, 0x1406 /*FLOAT*/, 0, 16, QUAD);
    }
    if (v->a_uv >= 0) {
        G.EnableVertexAttribArray((unsigned)v->a_uv);
        G.VertexAttribPointer((unsigned)v->a_uv, 2, 0x1406, 0, 16, QUAD + 2);
    }
    G.DrawArrays(0x0005 /*TRIANGLE_STRIP*/, 0, 4);
    int ok = e_SwapBuffers(v->dpy, v->surf);
    if (!ok && fails++ < 3) {
        BRIDGE_WARN("bridge: eglSwapBuffers(ANGLE) 失败 err=0x%x",
                       e_GetError ? (unsigned)e_GetError() : 0u);
    }
    e_MakeCurrent(v->dpy, (V)0, (V)0, (V)0);
    ab_set_busy(0);
    return ok;
}

void ab_view_destroy(void *st) {
    ab_view *v = (ab_view *)st;
    if (!v) return;
    ab_set_busy(1);
    if (e_DestroySurface && v->surf) {
        e_MakeCurrent(v->dpy, (V)0, (V)0, (V)0);
        e_DestroySurface(v->dpy, v->surf);
    }
    if (e_DestroyContext && v->ctx) { e_MakeCurrent(v->dpy, (V)0, (V)0, (V)0); e_DestroyContext(v->dpy, v->ctx); }
    ab_set_busy(0);
    free(v);
}
