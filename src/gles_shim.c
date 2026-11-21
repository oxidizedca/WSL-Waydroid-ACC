/* 自动生成：GLES2/GLES3 入口转发到 glibc Mesa（经 eglGetProcAddress） */
#include <GLES2/gl2.h>
#include <GLES3/gl3.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

/* 不能把 EGL 库列成 DT_NEEDED：集成时我们的 EGL 同时以 libEGL_angle.so 装着，
 * 链表会再按名字拉一份 libEGL_d3d12.so —— 同一进程两份 loader 状态、两份 TLS 表，
 * 结果 context 在一份里、GL 调用打到另一份（glGetString 返回 NULL）。
 * 改成运行时找"已经加载的那一份"。 */
static void *(*g_gl_proc)(const char *);
static int is_surfaceflinger(void);
/* egl_img_target_texture 也必须在【运行时】解析，不能留成 ELF 未定义符号。
 * 原因（第 35 轮定位）：meta-EGL 的 Loader::load_system_driver() 用
 * dlopen("/system/lib64/libGLESv2_angle.so", RTLD_NOW|RTLD_LOCAL) 加载 GLES 驱动，
 * 此时我们的 libEGL_angle.so 还不在全局符号域里；只要有一个未定义符号，dlopen 就
 * 整体失败 → load_system_driver 返回 NULL → initialize_api() 里 dlsym(NULL, name)
 * 退化成 dlsym(RTLD_DEFAULT)，把 gHooks 表填成 bionic stub 库自己的地址
 * （libGLESv1_CM.so / libGLESv2.so）→ stub 的尾跳 jmp *hooks 又跳回自己 →
 * 第一个 GL 调用（SF: glGetString(GL_VENDOR)）100% CPU 空转，SF 再也不回 binder。
 * 所以这里所有跨库符号都必须延后到 dlopen 之后的 dlsym 里拿。 */
static int (*g_img_target)(unsigned int, void *);
/* 第 53 轮：EGLImage 句柄改成真纹理 id 之后需要的三个钩子 */
static void (*g_img_flush_dirty)(void);
static void (*g_shim_capture)(void *);
static int  (*g_img_note_fbo)(unsigned int);
static void (*frame_encoder_output)(void);
static void shim_resolve_egl(void) {
    if (g_gl_proc) return;
    static const char *names[] = { "libEGL_angle.so", "libEGL_d3d12.so", NULL };
    for (int i = 0; names[i]; i++) {
        void *h = dlopen(names[i], RTLD_NOW);
        if (!h) continue;
        if (!g_gl_proc)    g_gl_proc    = (void *(*)(const char *))dlsym(h, "egl_shim_gl_proc");
        if (!g_img_target) g_img_target = (int (*)(unsigned int, void *))dlsym(h, "egl_img_target_texture");
        if (!g_img_flush_dirty) g_img_flush_dirty = (void (*)(void))dlsym(h, "egl_img_flush_dirty");
        if (!g_img_note_fbo)   g_img_note_fbo  = (int (*)(unsigned int))dlsym(h, "egl_img_note_fbo");
        frame_encoder_output=dlsym(h,"egl_t2_write_encoder");
        if (!g_shim_capture)   g_shim_capture  = (void (*)(void *))dlsym(h, "egl_shim_capture_frame");
        if (g_gl_proc) return;
    }
}
static void *shim_gl_proc(const char *name) { shim_resolve_egl(); return g_gl_proc ? g_gl_proc(name) : 0; }

/* GLES 转发库也要导出 eglGetProcAddress（第 38 轮，HWC 崩在 call 0x0 之后加的）：
 * meta-EGL 的 Loader 有两条分支会 dlsym(GLES 驱动, "eglGetProcAddress")——
 *   load_system_driver("GLES", suffix) 的"单库 libGLES_<suffix>.so"分支，
 *   以及 initialize_api(dso, EGL|GLESv1_CM|GLESv2)。
 * 拿不到就把 getProcAddress 存成 NULL，紧接着 init_api 里 call *%r13 直接 SIGSEGV
 * （tombstone: Loader::init_api+293 / attempt_to_load_system_driver+391 / ip=0）。
 * 所以这里补一个同名导出，转发到我们 EGL 库的实现（它再按进程分发）。 */
__attribute__((visibility("default")))
void *eglGetProcAddress(const char *name) {
    static void *(*f)(const char *);
    if (!f) {
        static const char *names[] = { "libEGL_angle.so", "libEGL_d3d12.so", NULL };
        for (int i = 0; names[i] && !f; i++) {
            void *h = dlopen(names[i], RTLD_NOW);
            if (h) f = (void *(*)(const char *))dlsym(h, "eglGetProcAddress");
        }
    }
    return f ? f(name) : 0;
}
#define GF(n) shim_gl_proc(n)
GL_APICALL void GL_APIENTRY glEGLImageTargetTexture2DOES(GLenum target, void *image) {
    shim_resolve_egl();
    if (g_img_target && g_img_target((unsigned)target, image)) return;
    static void (GL_APIENTRY *fn)(GLenum target, void *image);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum, void *))GF("glEGLImageTargetTexture2DOES");
    if (fn) fn(target, image);
}

/* glGetString 的返回值要能看到：Skia/HWUI 的 GL interface 校验依赖 VERSION/EXTENSIONS */
GL_APICALL const GLubyte *GL_APIENTRY glGetString(GLenum name) {
    static const GLubyte *(*fn)(GLenum);
    if (!fn) fn = (const GLubyte *(*)(GLenum))GF("glGetString");
    return fn ? fn(name) : NULL;
}

/* gles_stubs.S 里那些"懒解析蹦床"的统一入口：第一次被调用时按名字问 Mesa
 * 要 dispatch stub，再尾跳到真函数。补的是 gles_shim.c 里没手写的 ~609 个入口
 * （HWUI 一上来就 dlsym("glGetString")，以前直接拿到 NULL）。 */
__attribute__((visibility("default")))
void *shim_lazy_gl(const char *name) { return GF(name); }

GL_APICALL void GL_APIENTRY glActiveTexture(GLenum texture) {
    static void (GL_APIENTRY *fn)(GLenum texture);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum texture))GF("glActiveTexture");
    if (fn) fn(texture);
}

GL_APICALL void GL_APIENTRY glAttachShader(GLuint program, GLuint shader) {
    static void (GL_APIENTRY *fn)(GLuint program, GLuint shader);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint program, GLuint shader))GF("glAttachShader");
    if (fn) fn(program, shader);
}

GL_APICALL void GL_APIENTRY glBindAttribLocation(GLuint program, GLuint index, const GLchar *name) {
    static void (GL_APIENTRY *fn)(GLuint program, GLuint index, const GLchar *name);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint program, GLuint index, const GLchar *name))GF("glBindAttribLocation");
    if (fn) fn(program, index, name);
}

GL_APICALL void GL_APIENTRY glBindBuffer(GLenum target, GLuint buffer) {
    static void (GL_APIENTRY *fn)(GLenum target, GLuint buffer);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLuint buffer))GF("glBindBuffer");
    if (fn) fn(target, buffer);
}

GL_APICALL void GL_APIENTRY glBindFramebuffer(GLenum target, GLuint framebuffer) {
    static void (GL_APIENTRY *fn)(GLenum target, GLuint framebuffer);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLuint framebuffer))GF("glBindFramebuffer");
    if (fn) fn(target, framebuffer);
}

GL_APICALL void GL_APIENTRY glBindRenderbuffer(GLenum target, GLuint renderbuffer) {
    static void (GL_APIENTRY *fn)(GLenum target, GLuint renderbuffer);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLuint renderbuffer))GF("glBindRenderbuffer");
    if (fn) fn(target, renderbuffer);
}

GL_APICALL void GL_APIENTRY glBindTexture(GLenum target, GLuint texture) {
    static void (GL_APIENTRY *fn)(GLenum target, GLuint texture);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLuint texture))GF("glBindTexture");
    if (fn) fn(target, texture);
}

GL_APICALL void GL_APIENTRY glBlendColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) {
    static void (GL_APIENTRY *fn)(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
    if (!fn) fn = (void (GL_APIENTRY *)(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha))GF("glBlendColor");
    if (fn) fn(red, green, blue, alpha);
}

GL_APICALL void GL_APIENTRY glBlendEquation(GLenum mode) {
    static void (GL_APIENTRY *fn)(GLenum mode);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum mode))GF("glBlendEquation");
    if (fn) fn(mode);
}

GL_APICALL void GL_APIENTRY glBlendEquationSeparate(GLenum modeRGB, GLenum modeAlpha) {
    static void (GL_APIENTRY *fn)(GLenum modeRGB, GLenum modeAlpha);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum modeRGB, GLenum modeAlpha))GF("glBlendEquationSeparate");
    if (fn) fn(modeRGB, modeAlpha);
}

GL_APICALL void GL_APIENTRY glBlendFunc(GLenum sfactor, GLenum dfactor) {
    static void (GL_APIENTRY *fn)(GLenum sfactor, GLenum dfactor);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum sfactor, GLenum dfactor))GF("glBlendFunc");
    if (fn) fn(sfactor, dfactor);
}

GL_APICALL void GL_APIENTRY glBlendFuncSeparate(GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha) {
    static void (GL_APIENTRY *fn)(GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha))GF("glBlendFuncSeparate");
    if (fn) fn(sfactorRGB, dfactorRGB, sfactorAlpha, dfactorAlpha);
}

GL_APICALL void GL_APIENTRY glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage) {
    static void (GL_APIENTRY *fn)(GLenum target, GLsizeiptr size, const void *data, GLenum usage);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLsizeiptr size, const void *data, GLenum usage))GF("glBufferData");
    if (fn) fn(target, size, data, usage);
}

GL_APICALL void GL_APIENTRY glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data) {
    static void (GL_APIENTRY *fn)(GLenum target, GLintptr offset, GLsizeiptr size, const void *data);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLintptr offset, GLsizeiptr size, const void *data))GF("glBufferSubData");
    if (fn) fn(target, offset, size, data);
}

GL_APICALL GLenum GL_APIENTRY glCheckFramebufferStatus(GLenum target) {
    static GLenum (GL_APIENTRY *fn)(GLenum target);
    if (!fn) fn = (GLenum (GL_APIENTRY *)(GLenum target))GF("glCheckFramebufferStatus");
    return fn ? fn(target) : (GLenum)0;
}


GL_APICALL void GL_APIENTRY glClear(GLbitfield mask) {
    static void (GL_APIENTRY *fn)(GLbitfield mask);
    if (!fn) fn = (void (GL_APIENTRY *)(GLbitfield mask))GF("glClear");
    if (fn) fn(mask);
    if ((mask & GL_COLOR_BUFFER_BIT) && frame_encoder_output) frame_encoder_output();
}

GL_APICALL void GL_APIENTRY glClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) {
    static void (GL_APIENTRY *fn)(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
    if (!fn) fn = (void (GL_APIENTRY *)(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha))GF("glClearColor");
    if (fn) fn(red, green, blue, alpha);
}

GL_APICALL void GL_APIENTRY glClearDepthf(GLfloat d) {
    static void (GL_APIENTRY *fn)(GLfloat d);
    if (!fn) fn = (void (GL_APIENTRY *)(GLfloat d))GF("glClearDepthf");
    if (fn) fn(d);
}

GL_APICALL void GL_APIENTRY glClearStencil(GLint s) {
    static void (GL_APIENTRY *fn)(GLint s);
    if (!fn) fn = (void (GL_APIENTRY *)(GLint s))GF("glClearStencil");
    if (fn) fn(s);
}

GL_APICALL void GL_APIENTRY glColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha) {
    static void (GL_APIENTRY *fn)(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha);
    if (!fn) fn = (void (GL_APIENTRY *)(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha))GF("glColorMask");
    if (fn) fn(red, green, blue, alpha);
}

GL_APICALL void GL_APIENTRY glCompileShader(GLuint shader) {
    static void (GL_APIENTRY *fn)(GLuint shader);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint shader))GF("glCompileShader");
    if (fn) fn(shader);
}

GL_APICALL void GL_APIENTRY glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void *data) {
    static void (GL_APIENTRY *fn)(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void *data);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void *data))GF("glCompressedTexImage2D");
    if (fn) fn(target, level, internalformat, width, height, border, imageSize, data);
}

GL_APICALL void GL_APIENTRY glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, const void *data) {
    static void (GL_APIENTRY *fn)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, const void *data);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, const void *data))GF("glCompressedTexSubImage2D");
    if (fn) fn(target, level, xoffset, yoffset, width, height, format, imageSize, data);
}

GL_APICALL void GL_APIENTRY glCopyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border) {
    static void (GL_APIENTRY *fn)(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border))GF("glCopyTexImage2D");
    if (fn) fn(target, level, internalformat, x, y, width, height, border);
}

GL_APICALL void GL_APIENTRY glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height) {
    static void (GL_APIENTRY *fn)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height))GF("glCopyTexSubImage2D");
    if (fn) fn(target, level, xoffset, yoffset, x, y, width, height);
}

GL_APICALL GLuint GL_APIENTRY glCreateProgram(void) {
    static GLuint (GL_APIENTRY *fn)(void);
    if (!fn) fn = (GLuint (GL_APIENTRY *)(void))GF("glCreateProgram");
    return fn ? fn() : (GLuint)0;
}

GL_APICALL GLuint GL_APIENTRY glCreateShader(GLenum type) {
    static GLuint (GL_APIENTRY *fn)(GLenum type);
    if (!fn) fn = (GLuint (GL_APIENTRY *)(GLenum type))GF("glCreateShader");
    return fn ? fn(type) : (GLuint)0;
}

GL_APICALL void GL_APIENTRY glCullFace(GLenum mode) {
    static void (GL_APIENTRY *fn)(GLenum mode);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum mode))GF("glCullFace");
    if (fn) fn(mode);
}

GL_APICALL void GL_APIENTRY glDeleteBuffers(GLsizei n, const GLuint *buffers) {
    static void (GL_APIENTRY *fn)(GLsizei n, const GLuint *buffers);
    if (!fn) fn = (void (GL_APIENTRY *)(GLsizei n, const GLuint *buffers))GF("glDeleteBuffers");
    if (fn) fn(n, buffers);
}

GL_APICALL void GL_APIENTRY glDeleteFramebuffers(GLsizei n, const GLuint *framebuffers) {
    static void (GL_APIENTRY *fn)(GLsizei n, const GLuint *framebuffers);
    if (!fn) fn = (void (GL_APIENTRY *)(GLsizei n, const GLuint *framebuffers))GF("glDeleteFramebuffers");
    if (fn) fn(n, framebuffers);
}

GL_APICALL void GL_APIENTRY glDeleteProgram(GLuint program) {
    static void (GL_APIENTRY *fn)(GLuint program);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint program))GF("glDeleteProgram");
    if (fn) fn(program);
}

GL_APICALL void GL_APIENTRY glDeleteRenderbuffers(GLsizei n, const GLuint *renderbuffers) {
    static void (GL_APIENTRY *fn)(GLsizei n, const GLuint *renderbuffers);
    if (!fn) fn = (void (GL_APIENTRY *)(GLsizei n, const GLuint *renderbuffers))GF("glDeleteRenderbuffers");
    if (fn) fn(n, renderbuffers);
}

GL_APICALL void GL_APIENTRY glDeleteShader(GLuint shader) {
    static void (GL_APIENTRY *fn)(GLuint shader);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint shader))GF("glDeleteShader");
    if (fn) fn(shader);
}

GL_APICALL void GL_APIENTRY glDeleteTextures(GLsizei n, const GLuint *textures) {
    static void (GL_APIENTRY *fn)(GLsizei n, const GLuint *textures);
    if (!fn) fn = (void (GL_APIENTRY *)(GLsizei n, const GLuint *textures))GF("glDeleteTextures");
    if (fn) fn(n, textures);
}

GL_APICALL void GL_APIENTRY glDepthFunc(GLenum func) {
    static void (GL_APIENTRY *fn)(GLenum func);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum func))GF("glDepthFunc");
    if (fn) fn(func);
}

GL_APICALL void GL_APIENTRY glDepthMask(GLboolean flag) {
    static void (GL_APIENTRY *fn)(GLboolean flag);
    if (!fn) fn = (void (GL_APIENTRY *)(GLboolean flag))GF("glDepthMask");
    if (fn) fn(flag);
}

GL_APICALL void GL_APIENTRY glDepthRangef(GLfloat n, GLfloat f) {
    static void (GL_APIENTRY *fn)(GLfloat n, GLfloat f);
    if (!fn) fn = (void (GL_APIENTRY *)(GLfloat n, GLfloat f))GF("glDepthRangef");
    if (fn) fn(n, f);
}

GL_APICALL void GL_APIENTRY glDetachShader(GLuint program, GLuint shader) {
    static void (GL_APIENTRY *fn)(GLuint program, GLuint shader);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint program, GLuint shader))GF("glDetachShader");
    if (fn) fn(program, shader);
}

GL_APICALL void GL_APIENTRY glDisable(GLenum cap) {
    static void (GL_APIENTRY *fn)(GLenum cap);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum cap))GF("glDisable");
    if (fn) fn(cap);
}

GL_APICALL void GL_APIENTRY glDisableVertexAttribArray(GLuint index) {
    static void (GL_APIENTRY *fn)(GLuint index);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint index))GF("glDisableVertexAttribArray");
    if (fn) fn(index);
}

GL_APICALL void GL_APIENTRY glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    static void (GL_APIENTRY *fn)(GLenum mode, GLint first, GLsizei count);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum mode, GLint first, GLsizei count))GF("glDrawArrays");
    if (fn) fn(mode, first, count);
    if (frame_encoder_output) frame_encoder_output();
}

GL_APICALL void GL_APIENTRY glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) {
    static void (GL_APIENTRY *fn)(GLenum mode, GLsizei count, GLenum type, const void *indices);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum mode, GLsizei count, GLenum type, const void *indices))GF("glDrawElements");
    if (fn) fn(mode, count, type, indices);
    if (frame_encoder_output) frame_encoder_output();
}

GL_APICALL void GL_APIENTRY glEnable(GLenum cap) {
    static void (GL_APIENTRY *fn)(GLenum cap);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum cap))GF("glEnable");
    if (fn) fn(cap);
}

GL_APICALL void GL_APIENTRY glEnableVertexAttribArray(GLuint index) {
    static void (GL_APIENTRY *fn)(GLuint index);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint index))GF("glEnableVertexAttribArray");
    if (fn) fn(index);
}


GL_APICALL void GL_APIENTRY glFinish(void) {
    static void (GL_APIENTRY *fn)(void);
    if (!fn) fn = (void (GL_APIENTRY *)(void))GF("glFinish");
    if (fn) fn();
    shim_resolve_egl();
    /* [P0 实验] SF 的 RenderEngine 没有 window surface：它把 layer 渲染进 EGLImage 绑定的
     * gralloc buffer，永远不经过 eglSwapBuffers。我们这层如果不把纹理内容回写到 buffer，
     * buffer 就一直保持旧内容 —— SF 每帧算好的 alpha 写不进画面，动画于是「看不见」。
     * 这里在 glFinish 这个离屏渲染唯一的通用收尾点按 dirty 回写。
     * 注意：r74 还多一个每帧全量 force 回写（容器里 force_all_flush 文件在），
     * 那是「动画出现但壁纸黑」的来源；本版先只做 dirty 回写，用于区分两者。 */
    if (g_img_flush_dirty) g_img_flush_dirty();
    /* [P0] 只有 surfaceflinger 需要「每帧全量回写」：它的 RenderEngine 用持久 FBO，
     * dirty 永远不置位，不强制回写的话 layer buffer 一直停在旧内容 —— SF 算好的
     * alpha 写不进画面，逐帧事务动画（splash reveal / Activity 过渡）就看不见。
     * app 进程不做这一步：r74 在每个进程都全量回写，launcher 的壁纸纹理也被写回，
     * 就出现了「动画有了但壁纸黑」。 */
    if (is_surfaceflinger()) {
        static void (*ga)(void);
        if (!ga) {
            void *h = dlopen("libEGL_angle.so", RTLD_NOW);
            if (!h) h = dlopen("libEGL_d3d12.so", RTLD_NOW);
            if (h) ga = (void (*)(void))dlsym(h, "egl_img_flush_render_targets");
        }
        if (ga) {
            ga();
        }
    }
    if (g_shim_capture) {
        static void *(*cur)(int);
        if (!cur) {
            void *h = dlopen("libEGL_angle.so", RTLD_NOW);
            if (!h) h = dlopen("libEGL_d3d12.so", RTLD_NOW);
            if (h) cur = (void *(*)(int))dlsym(h, "eglGetCurrentSurface");
        }
        if (cur) {
            void *s = cur(0x3058 /*EGL_DRAW*/);
            if (s) g_shim_capture(s);
        }
    }
}

GL_APICALL void GL_APIENTRY glFlush(void) {
    static void (GL_APIENTRY *fn)(void);
    if (!fn) fn = (void (GL_APIENTRY *)(void))GF("glFlush");
    if (fn) fn();
    /* 第 53 轮补充 P：apps 不调 glFinish，只调 glFlush + fence。
     * 所以抓帧钩子挂在这里，节流（每 4 次抓 1 次），否则每帧多次 32MB 读回太重。 */
    static int cnt;
    if ((cnt++ & 3) == 0) {
        shim_resolve_egl();
        if (g_shim_capture) {
            static void *(*cur)(int);
            if (!cur) {
                void *h = dlopen("libEGL_angle.so", RTLD_NOW);
                if (!h) h = dlopen("libEGL_d3d12.so", RTLD_NOW);
                if (h) cur = (void *(*)(int))dlsym(h, "eglGetCurrentSurface");
            }
            if (cur) {
                void *s = cur(0x3058 /*EGL_DRAW*/);
                if (s) g_shim_capture(s);
            }
        }
    }
}

GL_APICALL void GL_APIENTRY glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer) {
    static void (GL_APIENTRY *fn)(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer))GF("glFramebufferRenderbuffer");
    if (fn) fn(target, attachment, renderbuffertarget, renderbuffer);
}

GL_APICALL void GL_APIENTRY glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {
    static void (GL_APIENTRY *fn)(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
    /* 我们的纹理被挂成附件 = 以后它就是渲染目标，内容会和 gralloc 脱钩，标记脏 */
    shim_resolve_egl();
    if (g_img_note_fbo && texture) g_img_note_fbo((unsigned)texture);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level))GF("glFramebufferTexture2D");
    if (fn) fn(target, attachment, textarget, texture, level);
}

GL_APICALL void GL_APIENTRY glFrontFace(GLenum mode) {
    static void (GL_APIENTRY *fn)(GLenum mode);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum mode))GF("glFrontFace");
    if (fn) fn(mode);
}

GL_APICALL void GL_APIENTRY glGenBuffers(GLsizei n, GLuint *buffers) {
    static void (GL_APIENTRY *fn)(GLsizei n, GLuint *buffers);
    if (!fn) fn = (void (GL_APIENTRY *)(GLsizei n, GLuint *buffers))GF("glGenBuffers");
    if (fn) fn(n, buffers);
}

GL_APICALL void GL_APIENTRY glGenerateMipmap(GLenum target) {
    static void (GL_APIENTRY *fn)(GLenum target);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target))GF("glGenerateMipmap");
    if (fn) fn(target);
}

GL_APICALL void GL_APIENTRY glGenFramebuffers(GLsizei n, GLuint *framebuffers) {
    static void (GL_APIENTRY *fn)(GLsizei n, GLuint *framebuffers);
    if (!fn) fn = (void (GL_APIENTRY *)(GLsizei n, GLuint *framebuffers))GF("glGenFramebuffers");
    if (fn) fn(n, framebuffers);
}

GL_APICALL void GL_APIENTRY glGenRenderbuffers(GLsizei n, GLuint *renderbuffers) {
    static void (GL_APIENTRY *fn)(GLsizei n, GLuint *renderbuffers);
    if (!fn) fn = (void (GL_APIENTRY *)(GLsizei n, GLuint *renderbuffers))GF("glGenRenderbuffers");
    if (fn) fn(n, renderbuffers);
}

GL_APICALL void GL_APIENTRY glGenTextures(GLsizei n, GLuint *textures) {
    static void (GL_APIENTRY *fn)(GLsizei n, GLuint *textures);
    if (!fn) fn = (void (GL_APIENTRY *)(GLsizei n, GLuint *textures))GF("glGenTextures");
    if (fn) fn(n, textures);
}

GL_APICALL void GL_APIENTRY glGetActiveAttrib(GLuint program, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name) {
    static void (GL_APIENTRY *fn)(GLuint program, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint program, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name))GF("glGetActiveAttrib");
    if (fn) fn(program, index, bufSize, length, size, type, name);
}

GL_APICALL void GL_APIENTRY glGetActiveUniform(GLuint program, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name) {
    static void (GL_APIENTRY *fn)(GLuint program, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint program, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name))GF("glGetActiveUniform");
    if (fn) fn(program, index, bufSize, length, size, type, name);
}

GL_APICALL void GL_APIENTRY glGetAttachedShaders(GLuint program, GLsizei maxCount, GLsizei *count, GLuint *shaders) {
    static void (GL_APIENTRY *fn)(GLuint program, GLsizei maxCount, GLsizei *count, GLuint *shaders);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint program, GLsizei maxCount, GLsizei *count, GLuint *shaders))GF("glGetAttachedShaders");
    if (fn) fn(program, maxCount, count, shaders);
}

GL_APICALL GLint GL_APIENTRY glGetAttribLocation(GLuint program, const GLchar *name) {
    static GLint (GL_APIENTRY *fn)(GLuint program, const GLchar *name);
    if (!fn) fn = (GLint (GL_APIENTRY *)(GLuint program, const GLchar *name))GF("glGetAttribLocation");
    return fn ? fn(program, name) : (GLint)0;
}

GL_APICALL void GL_APIENTRY glGetBooleanv(GLenum pname, GLboolean *data) {
    static void (GL_APIENTRY *fn)(GLenum pname, GLboolean *data);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum pname, GLboolean *data))GF("glGetBooleanv");
    if (fn) fn(pname, data);
}

GL_APICALL void GL_APIENTRY glGetBufferParameteriv(GLenum target, GLenum pname, GLint *params) {
    static void (GL_APIENTRY *fn)(GLenum target, GLenum pname, GLint *params);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLenum pname, GLint *params))GF("glGetBufferParameteriv");
    if (fn) fn(target, pname, params);
}

GL_APICALL GLenum GL_APIENTRY glGetError(void) {
    static GLenum (GL_APIENTRY *fn)(void);
    if (!fn) fn = (GLenum (GL_APIENTRY *)(void))GF("glGetError");
    return fn ? fn() : (GLenum)0;
}

GL_APICALL void GL_APIENTRY glGetFloatv(GLenum pname, GLfloat *data) {
    static void (GL_APIENTRY *fn)(GLenum pname, GLfloat *data);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum pname, GLfloat *data))GF("glGetFloatv");
    if (fn) fn(pname, data);
}

GL_APICALL void GL_APIENTRY glGetFramebufferAttachmentParameteriv(GLenum target, GLenum attachment, GLenum pname, GLint *params) {
    static void (GL_APIENTRY *fn)(GLenum target, GLenum attachment, GLenum pname, GLint *params);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLenum attachment, GLenum pname, GLint *params))GF("glGetFramebufferAttachmentParameteriv");
    if (fn) fn(target, attachment, pname, params);
}

GL_APICALL void GL_APIENTRY glGetIntegerv(GLenum pname, GLint *data) {
    static void (GL_APIENTRY *fn)(GLenum pname, GLint *data);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum pname, GLint *data))GF("glGetIntegerv");
    if (fn) fn(pname, data);
}

GL_APICALL void GL_APIENTRY glGetProgramiv(GLuint program, GLenum pname, GLint *params) {
    static void (GL_APIENTRY *fn)(GLuint program, GLenum pname, GLint *params);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint program, GLenum pname, GLint *params))GF("glGetProgramiv");
    if (fn) fn(program, pname, params);
}

GL_APICALL void GL_APIENTRY glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog) {
    static void (GL_APIENTRY *fn)(GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog))GF("glGetProgramInfoLog");
    if (fn) fn(program, bufSize, length, infoLog);
}

GL_APICALL void GL_APIENTRY glGetRenderbufferParameteriv(GLenum target, GLenum pname, GLint *params) {
    static void (GL_APIENTRY *fn)(GLenum target, GLenum pname, GLint *params);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLenum pname, GLint *params))GF("glGetRenderbufferParameteriv");
    if (fn) fn(target, pname, params);
}

GL_APICALL void GL_APIENTRY glGetShaderiv(GLuint shader, GLenum pname, GLint *params) {
    static void (GL_APIENTRY *fn)(GLuint shader, GLenum pname, GLint *params);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint shader, GLenum pname, GLint *params))GF("glGetShaderiv");
    if (fn) fn(shader, pname, params);
}

GL_APICALL void GL_APIENTRY glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog) {
    static void (GL_APIENTRY *fn)(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog))GF("glGetShaderInfoLog");
    if (fn) fn(shader, bufSize, length, infoLog);
}

GL_APICALL void GL_APIENTRY glGetShaderPrecisionFormat(GLenum shadertype, GLenum precisiontype, GLint *range, GLint *precision) {
    static void (GL_APIENTRY *fn)(GLenum shadertype, GLenum precisiontype, GLint *range, GLint *precision);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum shadertype, GLenum precisiontype, GLint *range, GLint *precision))GF("glGetShaderPrecisionFormat");
    if (fn) fn(shadertype, precisiontype, range, precision);
}

GL_APICALL void GL_APIENTRY glGetShaderSource(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *source) {
    static void (GL_APIENTRY *fn)(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *source);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *source))GF("glGetShaderSource");
    if (fn) fn(shader, bufSize, length, source);
}

GL_APICALL void GL_APIENTRY glGetTexParameterfv(GLenum target, GLenum pname, GLfloat *params) {
    static void (GL_APIENTRY *fn)(GLenum target, GLenum pname, GLfloat *params);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLenum pname, GLfloat *params))GF("glGetTexParameterfv");
    if (fn) fn(target, pname, params);
}

GL_APICALL void GL_APIENTRY glGetTexParameteriv(GLenum target, GLenum pname, GLint *params) {
    static void (GL_APIENTRY *fn)(GLenum target, GLenum pname, GLint *params);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLenum pname, GLint *params))GF("glGetTexParameteriv");
    if (fn) fn(target, pname, params);
}

GL_APICALL void GL_APIENTRY glGetUniformfv(GLuint program, GLint location, GLfloat *params) {
    static void (GL_APIENTRY *fn)(GLuint program, GLint location, GLfloat *params);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint program, GLint location, GLfloat *params))GF("glGetUniformfv");
    if (fn) fn(program, location, params);
}

GL_APICALL void GL_APIENTRY glGetUniformiv(GLuint program, GLint location, GLint *params) {
    static void (GL_APIENTRY *fn)(GLuint program, GLint location, GLint *params);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint program, GLint location, GLint *params))GF("glGetUniformiv");
    if (fn) fn(program, location, params);
}

GL_APICALL GLint GL_APIENTRY glGetUniformLocation(GLuint program, const GLchar *name) {
    static GLint (GL_APIENTRY *fn)(GLuint program, const GLchar *name);
    if (!fn) fn = (GLint (GL_APIENTRY *)(GLuint program, const GLchar *name))GF("glGetUniformLocation");
    return fn ? fn(program, name) : -1;
}

GL_APICALL void GL_APIENTRY glGetVertexAttribfv(GLuint index, GLenum pname, GLfloat *params) {
    static void (GL_APIENTRY *fn)(GLuint index, GLenum pname, GLfloat *params);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint index, GLenum pname, GLfloat *params))GF("glGetVertexAttribfv");
    if (fn) fn(index, pname, params);
}

GL_APICALL void GL_APIENTRY glGetVertexAttribiv(GLuint index, GLenum pname, GLint *params) {
    static void (GL_APIENTRY *fn)(GLuint index, GLenum pname, GLint *params);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint index, GLenum pname, GLint *params))GF("glGetVertexAttribiv");
    if (fn) fn(index, pname, params);
}

GL_APICALL void GL_APIENTRY glGetVertexAttribPointerv(GLuint index, GLenum pname, void **pointer) {
    static void (GL_APIENTRY *fn)(GLuint index, GLenum pname, void **pointer);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint index, GLenum pname, void **pointer))GF("glGetVertexAttribPointerv");
    if (fn) fn(index, pname, pointer);
}

GL_APICALL void GL_APIENTRY glHint(GLenum target, GLenum mode) {
    static void (GL_APIENTRY *fn)(GLenum target, GLenum mode);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLenum mode))GF("glHint");
    if (fn) fn(target, mode);
}

GL_APICALL GLboolean GL_APIENTRY glIsBuffer(GLuint buffer) {
    static GLboolean (GL_APIENTRY *fn)(GLuint buffer);
    if (!fn) fn = (GLboolean (GL_APIENTRY *)(GLuint buffer))GF("glIsBuffer");
    return fn ? fn(buffer) : (GLboolean)0;
}

GL_APICALL GLboolean GL_APIENTRY glIsEnabled(GLenum cap) {
    static GLboolean (GL_APIENTRY *fn)(GLenum cap);
    if (!fn) fn = (GLboolean (GL_APIENTRY *)(GLenum cap))GF("glIsEnabled");
    return fn ? fn(cap) : (GLboolean)0;
}

GL_APICALL GLboolean GL_APIENTRY glIsFramebuffer(GLuint framebuffer) {
    static GLboolean (GL_APIENTRY *fn)(GLuint framebuffer);
    if (!fn) fn = (GLboolean (GL_APIENTRY *)(GLuint framebuffer))GF("glIsFramebuffer");
    return fn ? fn(framebuffer) : (GLboolean)0;
}

GL_APICALL GLboolean GL_APIENTRY glIsProgram(GLuint program) {
    static GLboolean (GL_APIENTRY *fn)(GLuint program);
    if (!fn) fn = (GLboolean (GL_APIENTRY *)(GLuint program))GF("glIsProgram");
    return fn ? fn(program) : (GLboolean)0;
}

GL_APICALL GLboolean GL_APIENTRY glIsRenderbuffer(GLuint renderbuffer) {
    static GLboolean (GL_APIENTRY *fn)(GLuint renderbuffer);
    if (!fn) fn = (GLboolean (GL_APIENTRY *)(GLuint renderbuffer))GF("glIsRenderbuffer");
    return fn ? fn(renderbuffer) : (GLboolean)0;
}

GL_APICALL GLboolean GL_APIENTRY glIsShader(GLuint shader) {
    static GLboolean (GL_APIENTRY *fn)(GLuint shader);
    if (!fn) fn = (GLboolean (GL_APIENTRY *)(GLuint shader))GF("glIsShader");
    return fn ? fn(shader) : (GLboolean)0;
}

GL_APICALL GLboolean GL_APIENTRY glIsTexture(GLuint texture) {
    static GLboolean (GL_APIENTRY *fn)(GLuint texture);
    if (!fn) fn = (GLboolean (GL_APIENTRY *)(GLuint texture))GF("glIsTexture");
    return fn ? fn(texture) : (GLboolean)0;
}

GL_APICALL void GL_APIENTRY glLineWidth(GLfloat width) {
    static void (GL_APIENTRY *fn)(GLfloat width);
    if (!fn) fn = (void (GL_APIENTRY *)(GLfloat width))GF("glLineWidth");
    if (fn) fn(width);
}

GL_APICALL void GL_APIENTRY glLinkProgram(GLuint program) {
    static void (GL_APIENTRY *fn)(GLuint program);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint program))GF("glLinkProgram");
    if (fn) fn(program);
}

GL_APICALL void GL_APIENTRY glPixelStorei(GLenum pname, GLint param) {
    static void (GL_APIENTRY *fn)(GLenum pname, GLint param);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum pname, GLint param))GF("glPixelStorei");
    if (fn) fn(pname, param);
}

GL_APICALL void GL_APIENTRY glPolygonOffset(GLfloat factor, GLfloat units) {
    static void (GL_APIENTRY *fn)(GLfloat factor, GLfloat units);
    if (!fn) fn = (void (GL_APIENTRY *)(GLfloat factor, GLfloat units))GF("glPolygonOffset");
    if (fn) fn(factor, units);
}

GL_APICALL void GL_APIENTRY glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels) {
    static void (GL_APIENTRY *fn)(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels);
    if (!fn) fn = (void (GL_APIENTRY *)(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels))GF("glReadPixels");
    if (fn) fn(x, y, width, height, format, type, pixels);
}

GL_APICALL void GL_APIENTRY glReleaseShaderCompiler(void) {
    static void (GL_APIENTRY *fn)(void);
    if (!fn) fn = (void (GL_APIENTRY *)(void))GF("glReleaseShaderCompiler");
    if (fn) fn();
}

GL_APICALL void GL_APIENTRY glRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height) {
    static void (GL_APIENTRY *fn)(GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLenum internalformat, GLsizei width, GLsizei height))GF("glRenderbufferStorage");
    if (fn) fn(target, internalformat, width, height);
}

GL_APICALL void GL_APIENTRY glSampleCoverage(GLfloat value, GLboolean invert) {
    static void (GL_APIENTRY *fn)(GLfloat value, GLboolean invert);
    if (!fn) fn = (void (GL_APIENTRY *)(GLfloat value, GLboolean invert))GF("glSampleCoverage");
    if (fn) fn(value, invert);
}

GL_APICALL void GL_APIENTRY glScissor(GLint x, GLint y, GLsizei width, GLsizei height) {
    static void (GL_APIENTRY *fn)(GLint x, GLint y, GLsizei width, GLsizei height);
    if (!fn) fn = (void (GL_APIENTRY *)(GLint x, GLint y, GLsizei width, GLsizei height))GF("glScissor");
    if (fn) fn(x, y, width, height);
}

GL_APICALL void GL_APIENTRY glShaderBinary(GLsizei count, const GLuint *shaders, GLenum binaryformat, const void *binary, GLsizei length) {
    static void (GL_APIENTRY *fn)(GLsizei count, const GLuint *shaders, GLenum binaryformat, const void *binary, GLsizei length);
    if (!fn) fn = (void (GL_APIENTRY *)(GLsizei count, const GLuint *shaders, GLenum binaryformat, const void *binary, GLsizei length))GF("glShaderBinary");
    if (fn) fn(count, shaders, binaryformat, binary, length);
}


static int is_surfaceflinger(void) {
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd < 0) return 1;                 /* 判断不了就当它是，宁可不 dump */
    char b[256];
    ssize_t n = read(fd, b, sizeof b - 1);
    close(fd);
    if (n <= 0) return 1;
    b[n] = 0;
    return strstr(b, "surfaceflinger") != NULL;
}

GL_APICALL void GL_APIENTRY glShaderSource(GLuint shader, GLsizei count, const GLchar *const*string, const GLint *length) {
    static void (GL_APIENTRY *fn)(GLuint shader, GLsizei count, const GLchar *const*string, const GLint *length);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint shader, GLsizei count, const GLchar *const*string, const GLint *length))GF("glShaderSource");
    if (fn) fn(shader, count, string, length);
}

GL_APICALL void GL_APIENTRY glStencilFunc(GLenum func, GLint ref, GLuint mask) {
    static void (GL_APIENTRY *fn)(GLenum func, GLint ref, GLuint mask);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum func, GLint ref, GLuint mask))GF("glStencilFunc");
    if (fn) fn(func, ref, mask);
}

GL_APICALL void GL_APIENTRY glStencilFuncSeparate(GLenum face, GLenum func, GLint ref, GLuint mask) {
    static void (GL_APIENTRY *fn)(GLenum face, GLenum func, GLint ref, GLuint mask);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum face, GLenum func, GLint ref, GLuint mask))GF("glStencilFuncSeparate");
    if (fn) fn(face, func, ref, mask);
}

GL_APICALL void GL_APIENTRY glStencilMask(GLuint mask) {
    static void (GL_APIENTRY *fn)(GLuint mask);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint mask))GF("glStencilMask");
    if (fn) fn(mask);
}

GL_APICALL void GL_APIENTRY glStencilMaskSeparate(GLenum face, GLuint mask) {
    static void (GL_APIENTRY *fn)(GLenum face, GLuint mask);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum face, GLuint mask))GF("glStencilMaskSeparate");
    if (fn) fn(face, mask);
}

GL_APICALL void GL_APIENTRY glStencilOp(GLenum fail, GLenum zfail, GLenum zpass) {
    static void (GL_APIENTRY *fn)(GLenum fail, GLenum zfail, GLenum zpass);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum fail, GLenum zfail, GLenum zpass))GF("glStencilOp");
    if (fn) fn(fail, zfail, zpass);
}

GL_APICALL void GL_APIENTRY glStencilOpSeparate(GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass) {
    static void (GL_APIENTRY *fn)(GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass))GF("glStencilOpSeparate");
    if (fn) fn(face, sfail, dpfail, dppass);
}

GL_APICALL void GL_APIENTRY glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels) {
    static void (GL_APIENTRY *fn)(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels))GF("glTexImage2D");
    if (fn) fn(target, level, internalformat, width, height, border, format, type, pixels);
}

GL_APICALL void GL_APIENTRY glTexParameterf(GLenum target, GLenum pname, GLfloat param) {
    static void (GL_APIENTRY *fn)(GLenum target, GLenum pname, GLfloat param);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLenum pname, GLfloat param))GF("glTexParameterf");
    if (fn) fn(target, pname, param);
}

GL_APICALL void GL_APIENTRY glTexParameterfv(GLenum target, GLenum pname, const GLfloat *params) {
    static void (GL_APIENTRY *fn)(GLenum target, GLenum pname, const GLfloat *params);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLenum pname, const GLfloat *params))GF("glTexParameterfv");
    if (fn) fn(target, pname, params);
}

GL_APICALL void GL_APIENTRY glTexParameteri(GLenum target, GLenum pname, GLint param) {
    static void (GL_APIENTRY *fn)(GLenum target, GLenum pname, GLint param);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLenum pname, GLint param))GF("glTexParameteri");
    if (fn) fn(target, pname, param);
}

GL_APICALL void GL_APIENTRY glTexParameteriv(GLenum target, GLenum pname, const GLint *params) {
    static void (GL_APIENTRY *fn)(GLenum target, GLenum pname, const GLint *params);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLenum pname, const GLint *params))GF("glTexParameteriv");
    if (fn) fn(target, pname, params);
}

GL_APICALL void GL_APIENTRY glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels) {
    static void (GL_APIENTRY *fn)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels))GF("glTexSubImage2D");
    if (fn) fn(target, level, xoffset, yoffset, width, height, format, type, pixels);
}

GL_APICALL void GL_APIENTRY glUniform1f(GLint location, GLfloat v0) {
    static void (GL_APIENTRY *fn)(GLint location, GLfloat v0);
    if (!fn) fn = (void (GL_APIENTRY *)(GLint location, GLfloat v0))GF("glUniform1f");
    if (fn) fn(location, v0);
}

GL_APICALL void GL_APIENTRY glUniform1fv(GLint location, GLsizei count, const GLfloat *value) {
    static void (GL_APIENTRY *fn)(GLint location, GLsizei count, const GLfloat *value);
    if (!fn) fn = (void (GL_APIENTRY *)(GLint location, GLsizei count, const GLfloat *value))GF("glUniform1fv");
    if (fn) fn(location, count, value);
}

GL_APICALL void GL_APIENTRY glUniform1i(GLint location, GLint v0) {
    static void (GL_APIENTRY *fn)(GLint location, GLint v0);
    if (!fn) fn = (void (GL_APIENTRY *)(GLint location, GLint v0))GF("glUniform1i");
    if (fn) fn(location, v0);
}

GL_APICALL void GL_APIENTRY glUniform1iv(GLint location, GLsizei count, const GLint *value) {
    static void (GL_APIENTRY *fn)(GLint location, GLsizei count, const GLint *value);
    if (!fn) fn = (void (GL_APIENTRY *)(GLint location, GLsizei count, const GLint *value))GF("glUniform1iv");
    if (fn) fn(location, count, value);
}

GL_APICALL void GL_APIENTRY glUniform2f(GLint location, GLfloat v0, GLfloat v1) {
    static void (GL_APIENTRY *fn)(GLint location, GLfloat v0, GLfloat v1);
    if (!fn) fn = (void (GL_APIENTRY *)(GLint location, GLfloat v0, GLfloat v1))GF("glUniform2f");
    if (fn) fn(location, v0, v1);
}

GL_APICALL void GL_APIENTRY glUniform2fv(GLint location, GLsizei count, const GLfloat *value) {
    static void (GL_APIENTRY *fn)(GLint location, GLsizei count, const GLfloat *value);
    if (!fn) fn = (void (GL_APIENTRY *)(GLint location, GLsizei count, const GLfloat *value))GF("glUniform2fv");
    if (fn) fn(location, count, value);
}

GL_APICALL void GL_APIENTRY glUniform2i(GLint location, GLint v0, GLint v1) {
    static void (GL_APIENTRY *fn)(GLint location, GLint v0, GLint v1);
    if (!fn) fn = (void (GL_APIENTRY *)(GLint location, GLint v0, GLint v1))GF("glUniform2i");
    if (fn) fn(location, v0, v1);
}

GL_APICALL void GL_APIENTRY glUniform2iv(GLint location, GLsizei count, const GLint *value) {
    static void (GL_APIENTRY *fn)(GLint location, GLsizei count, const GLint *value);
    if (!fn) fn = (void (GL_APIENTRY *)(GLint location, GLsizei count, const GLint *value))GF("glUniform2iv");
    if (fn) fn(location, count, value);
}

GL_APICALL void GL_APIENTRY glUniform3f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2) {
    static void (GL_APIENTRY *fn)(GLint location, GLfloat v0, GLfloat v1, GLfloat v2);
    if (!fn) fn = (void (GL_APIENTRY *)(GLint location, GLfloat v0, GLfloat v1, GLfloat v2))GF("glUniform3f");
    if (fn) fn(location, v0, v1, v2);
}

GL_APICALL void GL_APIENTRY glUniform3fv(GLint location, GLsizei count, const GLfloat *value) {
    static void (GL_APIENTRY *fn)(GLint location, GLsizei count, const GLfloat *value);
    if (!fn) fn = (void (GL_APIENTRY *)(GLint location, GLsizei count, const GLfloat *value))GF("glUniform3fv");
    if (fn) fn(location, count, value);
}

GL_APICALL void GL_APIENTRY glUniform3i(GLint location, GLint v0, GLint v1, GLint v2) {
    static void (GL_APIENTRY *fn)(GLint location, GLint v0, GLint v1, GLint v2);
    if (!fn) fn = (void (GL_APIENTRY *)(GLint location, GLint v0, GLint v1, GLint v2))GF("glUniform3i");
    if (fn) fn(location, v0, v1, v2);
}

GL_APICALL void GL_APIENTRY glUniform3iv(GLint location, GLsizei count, const GLint *value) {
    static void (GL_APIENTRY *fn)(GLint location, GLsizei count, const GLint *value);
    if (!fn) fn = (void (GL_APIENTRY *)(GLint location, GLsizei count, const GLint *value))GF("glUniform3iv");
    if (fn) fn(location, count, value);
}

GL_APICALL void GL_APIENTRY glUniform4f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3) {
    static void (GL_APIENTRY *fn)(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3);
    if (!fn) fn = (void (GL_APIENTRY *)(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3))GF("glUniform4f");
    if (fn) fn(location, v0, v1, v2, v3);
}

GL_APICALL void GL_APIENTRY glUniform4fv(GLint location, GLsizei count, const GLfloat *value) {
    static void (GL_APIENTRY *fn)(GLint location, GLsizei count, const GLfloat *value);
    if (!fn) fn = (void (GL_APIENTRY *)(GLint location, GLsizei count, const GLfloat *value))GF("glUniform4fv");
    if (fn) fn(location, count, value);
}

GL_APICALL void GL_APIENTRY glUniform4i(GLint location, GLint v0, GLint v1, GLint v2, GLint v3) {
    static void (GL_APIENTRY *fn)(GLint location, GLint v0, GLint v1, GLint v2, GLint v3);
    if (!fn) fn = (void (GL_APIENTRY *)(GLint location, GLint v0, GLint v1, GLint v2, GLint v3))GF("glUniform4i");
    if (fn) fn(location, v0, v1, v2, v3);
}

GL_APICALL void GL_APIENTRY glUniform4iv(GLint location, GLsizei count, const GLint *value) {
    static void (GL_APIENTRY *fn)(GLint location, GLsizei count, const GLint *value);
    if (!fn) fn = (void (GL_APIENTRY *)(GLint location, GLsizei count, const GLint *value))GF("glUniform4iv");
    if (fn) fn(location, count, value);
}

GL_APICALL void GL_APIENTRY glUniformMatrix2fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
    static void (GL_APIENTRY *fn)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
    if (!fn) fn = (void (GL_APIENTRY *)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value))GF("glUniformMatrix2fv");
    if (fn) fn(location, count, transpose, value);
}

GL_APICALL void GL_APIENTRY glUniformMatrix3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
    static void (GL_APIENTRY *fn)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
    if (!fn) fn = (void (GL_APIENTRY *)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value))GF("glUniformMatrix3fv");
    if (fn) fn(location, count, transpose, value);
}

GL_APICALL void GL_APIENTRY glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
    static void (GL_APIENTRY *fn)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
    if (!fn) fn = (void (GL_APIENTRY *)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value))GF("glUniformMatrix4fv");
    if (fn) fn(location, count, transpose, value);
}

GL_APICALL void GL_APIENTRY glUseProgram(GLuint program) {
    static void (GL_APIENTRY *fn)(GLuint program);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint program))GF("glUseProgram");
    if (fn) fn(program);
}

GL_APICALL void GL_APIENTRY glValidateProgram(GLuint program) {
    static void (GL_APIENTRY *fn)(GLuint program);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint program))GF("glValidateProgram");
    if (fn) fn(program);
}

GL_APICALL void GL_APIENTRY glVertexAttrib1f(GLuint index, GLfloat x) {
    static void (GL_APIENTRY *fn)(GLuint index, GLfloat x);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint index, GLfloat x))GF("glVertexAttrib1f");
    if (fn) fn(index, x);
}

GL_APICALL void GL_APIENTRY glVertexAttrib1fv(GLuint index, const GLfloat *v) {
    static void (GL_APIENTRY *fn)(GLuint index, const GLfloat *v);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint index, const GLfloat *v))GF("glVertexAttrib1fv");
    if (fn) fn(index, v);
}

GL_APICALL void GL_APIENTRY glVertexAttrib2f(GLuint index, GLfloat x, GLfloat y) {
    static void (GL_APIENTRY *fn)(GLuint index, GLfloat x, GLfloat y);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint index, GLfloat x, GLfloat y))GF("glVertexAttrib2f");
    if (fn) fn(index, x, y);
}

GL_APICALL void GL_APIENTRY glVertexAttrib2fv(GLuint index, const GLfloat *v) {
    static void (GL_APIENTRY *fn)(GLuint index, const GLfloat *v);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint index, const GLfloat *v))GF("glVertexAttrib2fv");
    if (fn) fn(index, v);
}

GL_APICALL void GL_APIENTRY glVertexAttrib3f(GLuint index, GLfloat x, GLfloat y, GLfloat z) {
    static void (GL_APIENTRY *fn)(GLuint index, GLfloat x, GLfloat y, GLfloat z);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint index, GLfloat x, GLfloat y, GLfloat z))GF("glVertexAttrib3f");
    if (fn) fn(index, x, y, z);
}

GL_APICALL void GL_APIENTRY glVertexAttrib3fv(GLuint index, const GLfloat *v) {
    static void (GL_APIENTRY *fn)(GLuint index, const GLfloat *v);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint index, const GLfloat *v))GF("glVertexAttrib3fv");
    if (fn) fn(index, v);
}

GL_APICALL void GL_APIENTRY glVertexAttrib4f(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
    static void (GL_APIENTRY *fn)(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w))GF("glVertexAttrib4f");
    if (fn) fn(index, x, y, z, w);
}

GL_APICALL void GL_APIENTRY glVertexAttrib4fv(GLuint index, const GLfloat *v) {
    static void (GL_APIENTRY *fn)(GLuint index, const GLfloat *v);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint index, const GLfloat *v))GF("glVertexAttrib4fv");
    if (fn) fn(index, v);
}

GL_APICALL void GL_APIENTRY glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer) {
    static void (GL_APIENTRY *fn)(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer))GF("glVertexAttribPointer");
    if (fn) fn(index, size, type, normalized, stride, pointer);
}

GL_APICALL void GL_APIENTRY glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    static void (GL_APIENTRY *fn)(GLint x, GLint y, GLsizei width, GLsizei height);
    if (!fn) fn = (void (GL_APIENTRY *)(GLint x, GLint y, GLsizei width, GLsizei height))GF("glViewport");
    if (fn) fn(x, y, width, height);
}

GL_APICALL void GL_APIENTRY glReadBuffer(GLenum src) {
    static void (GL_APIENTRY *fn)(GLenum src);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum src))GF("glReadBuffer");
    if (fn) fn(src);
}

GL_APICALL void GL_APIENTRY glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void *indices) {
    static void (GL_APIENTRY *fn)(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void *indices);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void *indices))GF("glDrawRangeElements");
    if (fn) fn(mode, start, end, count, type, indices);
}

GL_APICALL void GL_APIENTRY glTexImage3D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type, const void *pixels) {
    static void (GL_APIENTRY *fn)(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type, const void *pixels);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type, const void *pixels))GF("glTexImage3D");
    if (fn) fn(target, level, internalformat, width, height, depth, border, format, type, pixels);
}

GL_APICALL void GL_APIENTRY glTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const void *pixels) {
    static void (GL_APIENTRY *fn)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const void *pixels);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const void *pixels))GF("glTexSubImage3D");
    if (fn) fn(target, level, xoffset, yoffset, zoffset, width, height, depth, format, type, pixels);
}

GL_APICALL void GL_APIENTRY glCopyTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLint x, GLint y, GLsizei width, GLsizei height) {
    static void (GL_APIENTRY *fn)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLint x, GLint y, GLsizei width, GLsizei height);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLint x, GLint y, GLsizei width, GLsizei height))GF("glCopyTexSubImage3D");
    if (fn) fn(target, level, xoffset, yoffset, zoffset, x, y, width, height);
}

GL_APICALL void GL_APIENTRY glCompressedTexImage3D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLsizei imageSize, const void *data) {
    static void (GL_APIENTRY *fn)(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLsizei imageSize, const void *data);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLsizei imageSize, const void *data))GF("glCompressedTexImage3D");
    if (fn) fn(target, level, internalformat, width, height, depth, border, imageSize, data);
}

GL_APICALL void GL_APIENTRY glCompressedTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLsizei imageSize, const void *data) {
    static void (GL_APIENTRY *fn)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLsizei imageSize, const void *data);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLsizei imageSize, const void *data))GF("glCompressedTexSubImage3D");
    if (fn) fn(target, level, xoffset, yoffset, zoffset, width, height, depth, format, imageSize, data);
}

GL_APICALL void GL_APIENTRY glGenQueries(GLsizei n, GLuint *ids) {
    static void (GL_APIENTRY *fn)(GLsizei n, GLuint *ids);
    if (!fn) fn = (void (GL_APIENTRY *)(GLsizei n, GLuint *ids))GF("glGenQueries");
    if (fn) fn(n, ids);
}

GL_APICALL void GL_APIENTRY glDeleteQueries(GLsizei n, const GLuint *ids) {
    static void (GL_APIENTRY *fn)(GLsizei n, const GLuint *ids);
    if (!fn) fn = (void (GL_APIENTRY *)(GLsizei n, const GLuint *ids))GF("glDeleteQueries");
    if (fn) fn(n, ids);
}

GL_APICALL GLboolean GL_APIENTRY glIsQuery(GLuint id) {
    static GLboolean (GL_APIENTRY *fn)(GLuint id);
    if (!fn) fn = (GLboolean (GL_APIENTRY *)(GLuint id))GF("glIsQuery");
    return fn ? fn(id) : (GLboolean)0;
}

GL_APICALL void GL_APIENTRY glBeginQuery(GLenum target, GLuint id) {
    static void (GL_APIENTRY *fn)(GLenum target, GLuint id);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLuint id))GF("glBeginQuery");
    if (fn) fn(target, id);
}

GL_APICALL void GL_APIENTRY glEndQuery(GLenum target) {
    static void (GL_APIENTRY *fn)(GLenum target);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target))GF("glEndQuery");
    if (fn) fn(target);
}

GL_APICALL void GL_APIENTRY glGetQueryiv(GLenum target, GLenum pname, GLint *params) {
    static void (GL_APIENTRY *fn)(GLenum target, GLenum pname, GLint *params);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLenum pname, GLint *params))GF("glGetQueryiv");
    if (fn) fn(target, pname, params);
}

GL_APICALL void GL_APIENTRY glGetQueryObjectuiv(GLuint id, GLenum pname, GLuint *params) {
    static void (GL_APIENTRY *fn)(GLuint id, GLenum pname, GLuint *params);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint id, GLenum pname, GLuint *params))GF("glGetQueryObjectuiv");
    if (fn) fn(id, pname, params);
}

GL_APICALL GLboolean GL_APIENTRY glUnmapBuffer(GLenum target) {
    static GLboolean (GL_APIENTRY *fn)(GLenum target);
    if (!fn) fn = (GLboolean (GL_APIENTRY *)(GLenum target))GF("glUnmapBuffer");
    return fn ? fn(target) : (GLboolean)0;
}

GL_APICALL void GL_APIENTRY glGetBufferPointerv(GLenum target, GLenum pname, void **params) {
    static void (GL_APIENTRY *fn)(GLenum target, GLenum pname, void **params);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLenum pname, void **params))GF("glGetBufferPointerv");
    if (fn) fn(target, pname, params);
}

GL_APICALL void GL_APIENTRY glDrawBuffers(GLsizei n, const GLenum *bufs) {
    static void (GL_APIENTRY *fn)(GLsizei n, const GLenum *bufs);
    if (!fn) fn = (void (GL_APIENTRY *)(GLsizei n, const GLenum *bufs))GF("glDrawBuffers");
    if (fn) fn(n, bufs);
}

GL_APICALL void GL_APIENTRY glUniformMatrix2x3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
    static void (GL_APIENTRY *fn)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
    if (!fn) fn = (void (GL_APIENTRY *)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value))GF("glUniformMatrix2x3fv");
    if (fn) fn(location, count, transpose, value);
}

GL_APICALL void GL_APIENTRY glUniformMatrix3x2fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
    static void (GL_APIENTRY *fn)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
    if (!fn) fn = (void (GL_APIENTRY *)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value))GF("glUniformMatrix3x2fv");
    if (fn) fn(location, count, transpose, value);
}

GL_APICALL void GL_APIENTRY glUniformMatrix2x4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
    static void (GL_APIENTRY *fn)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
    if (!fn) fn = (void (GL_APIENTRY *)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value))GF("glUniformMatrix2x4fv");
    if (fn) fn(location, count, transpose, value);
}

GL_APICALL void GL_APIENTRY glUniformMatrix4x2fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
    static void (GL_APIENTRY *fn)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
    if (!fn) fn = (void (GL_APIENTRY *)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value))GF("glUniformMatrix4x2fv");
    if (fn) fn(location, count, transpose, value);
}

GL_APICALL void GL_APIENTRY glUniformMatrix3x4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
    static void (GL_APIENTRY *fn)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
    if (!fn) fn = (void (GL_APIENTRY *)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value))GF("glUniformMatrix3x4fv");
    if (fn) fn(location, count, transpose, value);
}

GL_APICALL void GL_APIENTRY glUniformMatrix4x3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
    static void (GL_APIENTRY *fn)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
    if (!fn) fn = (void (GL_APIENTRY *)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value))GF("glUniformMatrix4x3fv");
    if (fn) fn(location, count, transpose, value);
}

GL_APICALL void GL_APIENTRY glBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter) {
    static void (GL_APIENTRY *fn)(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter);
    if (!fn) fn = (void (GL_APIENTRY *)(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter))GF("glBlitFramebuffer");
    if (fn) fn(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
    if ((mask & GL_COLOR_BUFFER_BIT) && frame_encoder_output) frame_encoder_output();
}

GL_APICALL void GL_APIENTRY glRenderbufferStorageMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height) {
    static void (GL_APIENTRY *fn)(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height))GF("glRenderbufferStorageMultisample");
    if (fn) fn(target, samples, internalformat, width, height);
}

GL_APICALL void GL_APIENTRY glFramebufferTextureLayer(GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer) {
    static void (GL_APIENTRY *fn)(GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer))GF("glFramebufferTextureLayer");
    if (fn) fn(target, attachment, texture, level, layer);
}

GL_APICALL void GL_APIENTRY glFlushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length) {
    static void (GL_APIENTRY *fn)(GLenum target, GLintptr offset, GLsizeiptr length);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLintptr offset, GLsizeiptr length))GF("glFlushMappedBufferRange");
    if (fn) fn(target, offset, length);
}

GL_APICALL void GL_APIENTRY glBindVertexArray(GLuint array) {
    static void (GL_APIENTRY *fn)(GLuint array);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint array))GF("glBindVertexArray");
    if (fn) fn(array);
}

GL_APICALL void GL_APIENTRY glDeleteVertexArrays(GLsizei n, const GLuint *arrays) {
    static void (GL_APIENTRY *fn)(GLsizei n, const GLuint *arrays);
    if (!fn) fn = (void (GL_APIENTRY *)(GLsizei n, const GLuint *arrays))GF("glDeleteVertexArrays");
    if (fn) fn(n, arrays);
}

GL_APICALL void GL_APIENTRY glGenVertexArrays(GLsizei n, GLuint *arrays) {
    static void (GL_APIENTRY *fn)(GLsizei n, GLuint *arrays);
    if (!fn) fn = (void (GL_APIENTRY *)(GLsizei n, GLuint *arrays))GF("glGenVertexArrays");
    if (fn) fn(n, arrays);
}

GL_APICALL GLboolean GL_APIENTRY glIsVertexArray(GLuint array) {
    static GLboolean (GL_APIENTRY *fn)(GLuint array);
    if (!fn) fn = (GLboolean (GL_APIENTRY *)(GLuint array))GF("glIsVertexArray");
    return fn ? fn(array) : (GLboolean)0;
}

GL_APICALL void GL_APIENTRY glGetIntegeri_v(GLenum target, GLuint index, GLint *data) {
    static void (GL_APIENTRY *fn)(GLenum target, GLuint index, GLint *data);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLuint index, GLint *data))GF("glGetIntegeri_v");
    if (fn) fn(target, index, data);
}

GL_APICALL void GL_APIENTRY glBeginTransformFeedback(GLenum primitiveMode) {
    static void (GL_APIENTRY *fn)(GLenum primitiveMode);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum primitiveMode))GF("glBeginTransformFeedback");
    if (fn) fn(primitiveMode);
}

GL_APICALL void GL_APIENTRY glEndTransformFeedback(void) {
    static void (GL_APIENTRY *fn)(void);
    if (!fn) fn = (void (GL_APIENTRY *)(void))GF("glEndTransformFeedback");
    if (fn) fn();
}

GL_APICALL void GL_APIENTRY glBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size) {
    static void (GL_APIENTRY *fn)(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size))GF("glBindBufferRange");
    if (fn) fn(target, index, buffer, offset, size);
}

GL_APICALL void GL_APIENTRY glBindBufferBase(GLenum target, GLuint index, GLuint buffer) {
    static void (GL_APIENTRY *fn)(GLenum target, GLuint index, GLuint buffer);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLuint index, GLuint buffer))GF("glBindBufferBase");
    if (fn) fn(target, index, buffer);
}

GL_APICALL void GL_APIENTRY glTransformFeedbackVaryings(GLuint program, GLsizei count, const GLchar *const*varyings, GLenum bufferMode) {
    static void (GL_APIENTRY *fn)(GLuint program, GLsizei count, const GLchar *const*varyings, GLenum bufferMode);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint program, GLsizei count, const GLchar *const*varyings, GLenum bufferMode))GF("glTransformFeedbackVaryings");
    if (fn) fn(program, count, varyings, bufferMode);
}

GL_APICALL void GL_APIENTRY glGetTransformFeedbackVarying(GLuint program, GLuint index, GLsizei bufSize, GLsizei *length, GLsizei *size, GLenum *type, GLchar *name) {
    static void (GL_APIENTRY *fn)(GLuint program, GLuint index, GLsizei bufSize, GLsizei *length, GLsizei *size, GLenum *type, GLchar *name);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint program, GLuint index, GLsizei bufSize, GLsizei *length, GLsizei *size, GLenum *type, GLchar *name))GF("glGetTransformFeedbackVarying");
    if (fn) fn(program, index, bufSize, length, size, type, name);
}

GL_APICALL void GL_APIENTRY glVertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride, const void *pointer) {
    static void (GL_APIENTRY *fn)(GLuint index, GLint size, GLenum type, GLsizei stride, const void *pointer);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint index, GLint size, GLenum type, GLsizei stride, const void *pointer))GF("glVertexAttribIPointer");
    if (fn) fn(index, size, type, stride, pointer);
}

GL_APICALL void GL_APIENTRY glGetVertexAttribIiv(GLuint index, GLenum pname, GLint *params) {
    static void (GL_APIENTRY *fn)(GLuint index, GLenum pname, GLint *params);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint index, GLenum pname, GLint *params))GF("glGetVertexAttribIiv");
    if (fn) fn(index, pname, params);
}

GL_APICALL void GL_APIENTRY glGetVertexAttribIuiv(GLuint index, GLenum pname, GLuint *params) {
    static void (GL_APIENTRY *fn)(GLuint index, GLenum pname, GLuint *params);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint index, GLenum pname, GLuint *params))GF("glGetVertexAttribIuiv");
    if (fn) fn(index, pname, params);
}

GL_APICALL void GL_APIENTRY glVertexAttribI4i(GLuint index, GLint x, GLint y, GLint z, GLint w) {
    static void (GL_APIENTRY *fn)(GLuint index, GLint x, GLint y, GLint z, GLint w);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint index, GLint x, GLint y, GLint z, GLint w))GF("glVertexAttribI4i");
    if (fn) fn(index, x, y, z, w);
}

GL_APICALL void GL_APIENTRY glVertexAttribI4ui(GLuint index, GLuint x, GLuint y, GLuint z, GLuint w) {
    static void (GL_APIENTRY *fn)(GLuint index, GLuint x, GLuint y, GLuint z, GLuint w);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint index, GLuint x, GLuint y, GLuint z, GLuint w))GF("glVertexAttribI4ui");
    if (fn) fn(index, x, y, z, w);
}

GL_APICALL void GL_APIENTRY glVertexAttribI4iv(GLuint index, const GLint *v) {
    static void (GL_APIENTRY *fn)(GLuint index, const GLint *v);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint index, const GLint *v))GF("glVertexAttribI4iv");
    if (fn) fn(index, v);
}

GL_APICALL void GL_APIENTRY glVertexAttribI4uiv(GLuint index, const GLuint *v) {
    static void (GL_APIENTRY *fn)(GLuint index, const GLuint *v);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint index, const GLuint *v))GF("glVertexAttribI4uiv");
    if (fn) fn(index, v);
}

GL_APICALL void GL_APIENTRY glGetUniformuiv(GLuint program, GLint location, GLuint *params) {
    static void (GL_APIENTRY *fn)(GLuint program, GLint location, GLuint *params);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint program, GLint location, GLuint *params))GF("glGetUniformuiv");
    if (fn) fn(program, location, params);
}

GL_APICALL GLint GL_APIENTRY glGetFragDataLocation(GLuint program, const GLchar *name) {
    static GLint (GL_APIENTRY *fn)(GLuint program, const GLchar *name);
    if (!fn) fn = (GLint (GL_APIENTRY *)(GLuint program, const GLchar *name))GF("glGetFragDataLocation");
    return fn ? fn(program, name) : (GLint)0;
}

GL_APICALL void GL_APIENTRY glUniform1ui(GLint location, GLuint v0) {
    static void (GL_APIENTRY *fn)(GLint location, GLuint v0);
    if (!fn) fn = (void (GL_APIENTRY *)(GLint location, GLuint v0))GF("glUniform1ui");
    if (fn) fn(location, v0);
}

GL_APICALL void GL_APIENTRY glUniform2ui(GLint location, GLuint v0, GLuint v1) {
    static void (GL_APIENTRY *fn)(GLint location, GLuint v0, GLuint v1);
    if (!fn) fn = (void (GL_APIENTRY *)(GLint location, GLuint v0, GLuint v1))GF("glUniform2ui");
    if (fn) fn(location, v0, v1);
}

GL_APICALL void GL_APIENTRY glUniform3ui(GLint location, GLuint v0, GLuint v1, GLuint v2) {
    static void (GL_APIENTRY *fn)(GLint location, GLuint v0, GLuint v1, GLuint v2);
    if (!fn) fn = (void (GL_APIENTRY *)(GLint location, GLuint v0, GLuint v1, GLuint v2))GF("glUniform3ui");
    if (fn) fn(location, v0, v1, v2);
}

GL_APICALL void GL_APIENTRY glUniform4ui(GLint location, GLuint v0, GLuint v1, GLuint v2, GLuint v3) {
    static void (GL_APIENTRY *fn)(GLint location, GLuint v0, GLuint v1, GLuint v2, GLuint v3);
    if (!fn) fn = (void (GL_APIENTRY *)(GLint location, GLuint v0, GLuint v1, GLuint v2, GLuint v3))GF("glUniform4ui");
    if (fn) fn(location, v0, v1, v2, v3);
}

GL_APICALL void GL_APIENTRY glUniform1uiv(GLint location, GLsizei count, const GLuint *value) {
    static void (GL_APIENTRY *fn)(GLint location, GLsizei count, const GLuint *value);
    if (!fn) fn = (void (GL_APIENTRY *)(GLint location, GLsizei count, const GLuint *value))GF("glUniform1uiv");
    if (fn) fn(location, count, value);
}

GL_APICALL void GL_APIENTRY glUniform2uiv(GLint location, GLsizei count, const GLuint *value) {
    static void (GL_APIENTRY *fn)(GLint location, GLsizei count, const GLuint *value);
    if (!fn) fn = (void (GL_APIENTRY *)(GLint location, GLsizei count, const GLuint *value))GF("glUniform2uiv");
    if (fn) fn(location, count, value);
}

GL_APICALL void GL_APIENTRY glUniform3uiv(GLint location, GLsizei count, const GLuint *value) {
    static void (GL_APIENTRY *fn)(GLint location, GLsizei count, const GLuint *value);
    if (!fn) fn = (void (GL_APIENTRY *)(GLint location, GLsizei count, const GLuint *value))GF("glUniform3uiv");
    if (fn) fn(location, count, value);
}

GL_APICALL void GL_APIENTRY glUniform4uiv(GLint location, GLsizei count, const GLuint *value) {
    static void (GL_APIENTRY *fn)(GLint location, GLsizei count, const GLuint *value);
    if (!fn) fn = (void (GL_APIENTRY *)(GLint location, GLsizei count, const GLuint *value))GF("glUniform4uiv");
    if (fn) fn(location, count, value);
}

GL_APICALL void GL_APIENTRY glClearBufferiv(GLenum buffer, GLint drawbuffer, const GLint *value) {
    static void (GL_APIENTRY *fn)(GLenum buffer, GLint drawbuffer, const GLint *value);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum buffer, GLint drawbuffer, const GLint *value))GF("glClearBufferiv");
    if (fn) fn(buffer, drawbuffer, value);
}

GL_APICALL void GL_APIENTRY glClearBufferuiv(GLenum buffer, GLint drawbuffer, const GLuint *value) {
    static void (GL_APIENTRY *fn)(GLenum buffer, GLint drawbuffer, const GLuint *value);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum buffer, GLint drawbuffer, const GLuint *value))GF("glClearBufferuiv");
    if (fn) fn(buffer, drawbuffer, value);
}

GL_APICALL void GL_APIENTRY glClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat *value) {
    static void (GL_APIENTRY *fn)(GLenum buffer, GLint drawbuffer, const GLfloat *value);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum buffer, GLint drawbuffer, const GLfloat *value))GF("glClearBufferfv");
    if (fn) fn(buffer, drawbuffer, value);
}

GL_APICALL void GL_APIENTRY glClearBufferfi(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil) {
    static void (GL_APIENTRY *fn)(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil))GF("glClearBufferfi");
    if (fn) fn(buffer, drawbuffer, depth, stencil);
}

GL_APICALL void GL_APIENTRY glCopyBufferSubData(GLenum readTarget, GLenum writeTarget, GLintptr readOffset, GLintptr writeOffset, GLsizeiptr size) {
    static void (GL_APIENTRY *fn)(GLenum readTarget, GLenum writeTarget, GLintptr readOffset, GLintptr writeOffset, GLsizeiptr size);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum readTarget, GLenum writeTarget, GLintptr readOffset, GLintptr writeOffset, GLsizeiptr size))GF("glCopyBufferSubData");
    if (fn) fn(readTarget, writeTarget, readOffset, writeOffset, size);
}

GL_APICALL void GL_APIENTRY glGetUniformIndices(GLuint program, GLsizei uniformCount, const GLchar *const*uniformNames, GLuint *uniformIndices) {
    static void (GL_APIENTRY *fn)(GLuint program, GLsizei uniformCount, const GLchar *const*uniformNames, GLuint *uniformIndices);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint program, GLsizei uniformCount, const GLchar *const*uniformNames, GLuint *uniformIndices))GF("glGetUniformIndices");
    if (fn) fn(program, uniformCount, uniformNames, uniformIndices);
}

GL_APICALL void GL_APIENTRY glGetActiveUniformsiv(GLuint program, GLsizei uniformCount, const GLuint *uniformIndices, GLenum pname, GLint *params) {
    static void (GL_APIENTRY *fn)(GLuint program, GLsizei uniformCount, const GLuint *uniformIndices, GLenum pname, GLint *params);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint program, GLsizei uniformCount, const GLuint *uniformIndices, GLenum pname, GLint *params))GF("glGetActiveUniformsiv");
    if (fn) fn(program, uniformCount, uniformIndices, pname, params);
}

GL_APICALL GLuint GL_APIENTRY glGetUniformBlockIndex(GLuint program, const GLchar *uniformBlockName) {
    static GLuint (GL_APIENTRY *fn)(GLuint program, const GLchar *uniformBlockName);
    if (!fn) fn = (GLuint (GL_APIENTRY *)(GLuint program, const GLchar *uniformBlockName))GF("glGetUniformBlockIndex");
    return fn ? fn(program, uniformBlockName) : (GLuint)0;
}

GL_APICALL void GL_APIENTRY glGetActiveUniformBlockiv(GLuint program, GLuint uniformBlockIndex, GLenum pname, GLint *params) {
    static void (GL_APIENTRY *fn)(GLuint program, GLuint uniformBlockIndex, GLenum pname, GLint *params);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint program, GLuint uniformBlockIndex, GLenum pname, GLint *params))GF("glGetActiveUniformBlockiv");
    if (fn) fn(program, uniformBlockIndex, pname, params);
}

GL_APICALL void GL_APIENTRY glGetActiveUniformBlockName(GLuint program, GLuint uniformBlockIndex, GLsizei bufSize, GLsizei *length, GLchar *uniformBlockName) {
    static void (GL_APIENTRY *fn)(GLuint program, GLuint uniformBlockIndex, GLsizei bufSize, GLsizei *length, GLchar *uniformBlockName);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint program, GLuint uniformBlockIndex, GLsizei bufSize, GLsizei *length, GLchar *uniformBlockName))GF("glGetActiveUniformBlockName");
    if (fn) fn(program, uniformBlockIndex, bufSize, length, uniformBlockName);
}

GL_APICALL void GL_APIENTRY glUniformBlockBinding(GLuint program, GLuint uniformBlockIndex, GLuint uniformBlockBinding) {
    static void (GL_APIENTRY *fn)(GLuint program, GLuint uniformBlockIndex, GLuint uniformBlockBinding);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint program, GLuint uniformBlockIndex, GLuint uniformBlockBinding))GF("glUniformBlockBinding");
    if (fn) fn(program, uniformBlockIndex, uniformBlockBinding);
}

GL_APICALL void GL_APIENTRY glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instancecount) {
    static void (GL_APIENTRY *fn)(GLenum mode, GLint first, GLsizei count, GLsizei instancecount);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum mode, GLint first, GLsizei count, GLsizei instancecount))GF("glDrawArraysInstanced");
    if (fn) fn(mode, first, count, instancecount);
    if (frame_encoder_output) frame_encoder_output();
}

GL_APICALL void GL_APIENTRY glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei instancecount) {
    static void (GL_APIENTRY *fn)(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei instancecount);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei instancecount))GF("glDrawElementsInstanced");
    if (fn) fn(mode, count, type, indices, instancecount);
    if (frame_encoder_output) frame_encoder_output();
}

GL_APICALL GLsync GL_APIENTRY glFenceSync(GLenum condition, GLbitfield flags) {
    static GLsync (GL_APIENTRY *fn)(GLenum condition, GLbitfield flags);
    if (!fn) fn = (GLsync (GL_APIENTRY *)(GLenum condition, GLbitfield flags))GF("glFenceSync");
    return fn ? fn(condition, flags) : (GLsync)0;
}

GL_APICALL GLboolean GL_APIENTRY glIsSync(GLsync sync) {
    static GLboolean (GL_APIENTRY *fn)(GLsync sync);
    if (!fn) fn = (GLboolean (GL_APIENTRY *)(GLsync sync))GF("glIsSync");
    return fn ? fn(sync) : (GLboolean)0;
}

GL_APICALL void GL_APIENTRY glDeleteSync(GLsync sync) {
    static void (GL_APIENTRY *fn)(GLsync sync);
    if (!fn) fn = (void (GL_APIENTRY *)(GLsync sync))GF("glDeleteSync");
    if (fn) fn(sync);
}

GL_APICALL GLenum GL_APIENTRY glClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout) {
    static GLenum (GL_APIENTRY *fn)(GLsync sync, GLbitfield flags, GLuint64 timeout);
    if (!fn) fn = (GLenum (GL_APIENTRY *)(GLsync sync, GLbitfield flags, GLuint64 timeout))GF("glClientWaitSync");
    return fn ? fn(sync, flags, timeout) : (GLenum)0;
}

GL_APICALL void GL_APIENTRY glWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout) {
    static void (GL_APIENTRY *fn)(GLsync sync, GLbitfield flags, GLuint64 timeout);
    if (!fn) fn = (void (GL_APIENTRY *)(GLsync sync, GLbitfield flags, GLuint64 timeout))GF("glWaitSync");
    if (fn) fn(sync, flags, timeout);
}

GL_APICALL void GL_APIENTRY glGetInteger64v(GLenum pname, GLint64 *data) {
    static void (GL_APIENTRY *fn)(GLenum pname, GLint64 *data);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum pname, GLint64 *data))GF("glGetInteger64v");
    if (fn) fn(pname, data);
}

GL_APICALL void GL_APIENTRY glGetSynciv(GLsync sync, GLenum pname, GLsizei bufSize, GLsizei *length, GLint *values) {
    static void (GL_APIENTRY *fn)(GLsync sync, GLenum pname, GLsizei bufSize, GLsizei *length, GLint *values);
    if (!fn) fn = (void (GL_APIENTRY *)(GLsync sync, GLenum pname, GLsizei bufSize, GLsizei *length, GLint *values))GF("glGetSynciv");
    if (fn) fn(sync, pname, bufSize, length, values);
}

GL_APICALL void GL_APIENTRY glGetInteger64i_v(GLenum target, GLuint index, GLint64 *data) {
    static void (GL_APIENTRY *fn)(GLenum target, GLuint index, GLint64 *data);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLuint index, GLint64 *data))GF("glGetInteger64i_v");
    if (fn) fn(target, index, data);
}

GL_APICALL void GL_APIENTRY glGetBufferParameteri64v(GLenum target, GLenum pname, GLint64 *params) {
    static void (GL_APIENTRY *fn)(GLenum target, GLenum pname, GLint64 *params);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLenum pname, GLint64 *params))GF("glGetBufferParameteri64v");
    if (fn) fn(target, pname, params);
}

GL_APICALL void GL_APIENTRY glGenSamplers(GLsizei count, GLuint *samplers) {
    static void (GL_APIENTRY *fn)(GLsizei count, GLuint *samplers);
    if (!fn) fn = (void (GL_APIENTRY *)(GLsizei count, GLuint *samplers))GF("glGenSamplers");
    if (fn) fn(count, samplers);
}

GL_APICALL void GL_APIENTRY glDeleteSamplers(GLsizei count, const GLuint *samplers) {
    static void (GL_APIENTRY *fn)(GLsizei count, const GLuint *samplers);
    if (!fn) fn = (void (GL_APIENTRY *)(GLsizei count, const GLuint *samplers))GF("glDeleteSamplers");
    if (fn) fn(count, samplers);
}

GL_APICALL GLboolean GL_APIENTRY glIsSampler(GLuint sampler) {
    static GLboolean (GL_APIENTRY *fn)(GLuint sampler);
    if (!fn) fn = (GLboolean (GL_APIENTRY *)(GLuint sampler))GF("glIsSampler");
    return fn ? fn(sampler) : (GLboolean)0;
}

GL_APICALL void GL_APIENTRY glBindSampler(GLuint unit, GLuint sampler) {
    static void (GL_APIENTRY *fn)(GLuint unit, GLuint sampler);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint unit, GLuint sampler))GF("glBindSampler");
    if (fn) fn(unit, sampler);
}

GL_APICALL void GL_APIENTRY glSamplerParameteri(GLuint sampler, GLenum pname, GLint param) {
    static void (GL_APIENTRY *fn)(GLuint sampler, GLenum pname, GLint param);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint sampler, GLenum pname, GLint param))GF("glSamplerParameteri");
    if (fn) fn(sampler, pname, param);
}

GL_APICALL void GL_APIENTRY glSamplerParameteriv(GLuint sampler, GLenum pname, const GLint *param) {
    static void (GL_APIENTRY *fn)(GLuint sampler, GLenum pname, const GLint *param);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint sampler, GLenum pname, const GLint *param))GF("glSamplerParameteriv");
    if (fn) fn(sampler, pname, param);
}

GL_APICALL void GL_APIENTRY glSamplerParameterf(GLuint sampler, GLenum pname, GLfloat param) {
    static void (GL_APIENTRY *fn)(GLuint sampler, GLenum pname, GLfloat param);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint sampler, GLenum pname, GLfloat param))GF("glSamplerParameterf");
    if (fn) fn(sampler, pname, param);
}

GL_APICALL void GL_APIENTRY glSamplerParameterfv(GLuint sampler, GLenum pname, const GLfloat *param) {
    static void (GL_APIENTRY *fn)(GLuint sampler, GLenum pname, const GLfloat *param);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint sampler, GLenum pname, const GLfloat *param))GF("glSamplerParameterfv");
    if (fn) fn(sampler, pname, param);
}

GL_APICALL void GL_APIENTRY glGetSamplerParameteriv(GLuint sampler, GLenum pname, GLint *params) {
    static void (GL_APIENTRY *fn)(GLuint sampler, GLenum pname, GLint *params);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint sampler, GLenum pname, GLint *params))GF("glGetSamplerParameteriv");
    if (fn) fn(sampler, pname, params);
}

GL_APICALL void GL_APIENTRY glGetSamplerParameterfv(GLuint sampler, GLenum pname, GLfloat *params) {
    static void (GL_APIENTRY *fn)(GLuint sampler, GLenum pname, GLfloat *params);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint sampler, GLenum pname, GLfloat *params))GF("glGetSamplerParameterfv");
    if (fn) fn(sampler, pname, params);
}

GL_APICALL void GL_APIENTRY glVertexAttribDivisor(GLuint index, GLuint divisor) {
    static void (GL_APIENTRY *fn)(GLuint index, GLuint divisor);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint index, GLuint divisor))GF("glVertexAttribDivisor");
    if (fn) fn(index, divisor);
}

GL_APICALL void GL_APIENTRY glBindTransformFeedback(GLenum target, GLuint id) {
    static void (GL_APIENTRY *fn)(GLenum target, GLuint id);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLuint id))GF("glBindTransformFeedback");
    if (fn) fn(target, id);
}

GL_APICALL void GL_APIENTRY glDeleteTransformFeedbacks(GLsizei n, const GLuint *ids) {
    static void (GL_APIENTRY *fn)(GLsizei n, const GLuint *ids);
    if (!fn) fn = (void (GL_APIENTRY *)(GLsizei n, const GLuint *ids))GF("glDeleteTransformFeedbacks");
    if (fn) fn(n, ids);
}

GL_APICALL void GL_APIENTRY glGenTransformFeedbacks(GLsizei n, GLuint *ids) {
    static void (GL_APIENTRY *fn)(GLsizei n, GLuint *ids);
    if (!fn) fn = (void (GL_APIENTRY *)(GLsizei n, GLuint *ids))GF("glGenTransformFeedbacks");
    if (fn) fn(n, ids);
}

GL_APICALL GLboolean GL_APIENTRY glIsTransformFeedback(GLuint id) {
    static GLboolean (GL_APIENTRY *fn)(GLuint id);
    if (!fn) fn = (GLboolean (GL_APIENTRY *)(GLuint id))GF("glIsTransformFeedback");
    return fn ? fn(id) : (GLboolean)0;
}

GL_APICALL void GL_APIENTRY glPauseTransformFeedback(void) {
    static void (GL_APIENTRY *fn)(void);
    if (!fn) fn = (void (GL_APIENTRY *)(void))GF("glPauseTransformFeedback");
    if (fn) fn();
}

GL_APICALL void GL_APIENTRY glResumeTransformFeedback(void) {
    static void (GL_APIENTRY *fn)(void);
    if (!fn) fn = (void (GL_APIENTRY *)(void))GF("glResumeTransformFeedback");
    if (fn) fn();
}

GL_APICALL void GL_APIENTRY glGetProgramBinary(GLuint program, GLsizei bufSize, GLsizei *length, GLenum *binaryFormat, void *binary) {
    static void (GL_APIENTRY *fn)(GLuint program, GLsizei bufSize, GLsizei *length, GLenum *binaryFormat, void *binary);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint program, GLsizei bufSize, GLsizei *length, GLenum *binaryFormat, void *binary))GF("glGetProgramBinary");
    if (fn) fn(program, bufSize, length, binaryFormat, binary);
}

GL_APICALL void GL_APIENTRY glProgramBinary(GLuint program, GLenum binaryFormat, const void *binary, GLsizei length) {
    static void (GL_APIENTRY *fn)(GLuint program, GLenum binaryFormat, const void *binary, GLsizei length);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint program, GLenum binaryFormat, const void *binary, GLsizei length))GF("glProgramBinary");
    if (fn) fn(program, binaryFormat, binary, length);
}

GL_APICALL void GL_APIENTRY glProgramParameteri(GLuint program, GLenum pname, GLint value) {
    static void (GL_APIENTRY *fn)(GLuint program, GLenum pname, GLint value);
    if (!fn) fn = (void (GL_APIENTRY *)(GLuint program, GLenum pname, GLint value))GF("glProgramParameteri");
    if (fn) fn(program, pname, value);
}

GL_APICALL void GL_APIENTRY glInvalidateFramebuffer(GLenum target, GLsizei numAttachments, const GLenum *attachments) {
    static void (GL_APIENTRY *fn)(GLenum target, GLsizei numAttachments, const GLenum *attachments);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLsizei numAttachments, const GLenum *attachments))GF("glInvalidateFramebuffer");
    if (fn) fn(target, numAttachments, attachments);
}

GL_APICALL void GL_APIENTRY glInvalidateSubFramebuffer(GLenum target, GLsizei numAttachments, const GLenum *attachments, GLint x, GLint y, GLsizei width, GLsizei height) {
    static void (GL_APIENTRY *fn)(GLenum target, GLsizei numAttachments, const GLenum *attachments, GLint x, GLint y, GLsizei width, GLsizei height);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLsizei numAttachments, const GLenum *attachments, GLint x, GLint y, GLsizei width, GLsizei height))GF("glInvalidateSubFramebuffer");
    if (fn) fn(target, numAttachments, attachments, x, y, width, height);
}

GL_APICALL void GL_APIENTRY glTexStorage2D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height) {
    static void (GL_APIENTRY *fn)(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height))GF("glTexStorage2D");
    if (fn) fn(target, levels, internalformat, width, height);
}

GL_APICALL void GL_APIENTRY glTexStorage3D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth) {
    static void (GL_APIENTRY *fn)(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth))GF("glTexStorage3D");
    if (fn) fn(target, levels, internalformat, width, height, depth);
}

GL_APICALL void GL_APIENTRY glGetInternalformativ(GLenum target, GLenum internalformat, GLenum pname, GLsizei bufSize, GLint *params) {
    static void (GL_APIENTRY *fn)(GLenum target, GLenum internalformat, GLenum pname, GLsizei bufSize, GLint *params);
    if (!fn) fn = (void (GL_APIENTRY *)(GLenum target, GLenum internalformat, GLenum pname, GLsizei bufSize, GLint *params))GF("glGetInternalformativ");
    if (fn) fn(target, internalformat, pname, bufSize, params);
}

