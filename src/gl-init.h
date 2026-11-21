#define _GNU_SOURCE
#include "frame.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/dma-buf.h>
#include <signal.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>

/* GL entry points from the same Mesa build that owns this desktop context. */
#define GL_PROCS(X) \
    X(PFNGLCREATEMEMORYOBJECTSEXTPROC, CreateMemoryObjectsEXT) \
    X(PFNGLMEMORYOBJECTPARAMETERIVEXTPROC, MemoryObjectParameterivEXT) \
    X(PFNGLIMPORTMEMORYFDEXTPROC, ImportMemoryFdEXT) \
    X(PFNGLTEXSTORAGEMEM2DEXTPROC, TexStorageMem2DEXT) \
    X(PFNGLDELETEMEMORYOBJECTSEXTPROC, DeleteMemoryObjectsEXT) \
    X(PFNGLGENFRAMEBUFFERSPROC, GenFramebuffers) \
    X(PFNGLBINDFRAMEBUFFERPROC, BindFramebuffer) \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, FramebufferTexture2D) \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, CheckFramebufferStatus) \
    X(PFNGLDELETEFRAMEBUFFERSPROC, DeleteFramebuffers) \
    X(__typeof__(&glGetString), GetString) \
    X(__typeof__(&glGetError), GetError) \
    X(__typeof__(&glBindTexture), BindTexture) \
    X(__typeof__(&glGenTextures), GenTextures) \
    X(__typeof__(&glDeleteTextures), DeleteTextures) \
    X(__typeof__(&glPixelStorei), PixelStorei) \
    X(__typeof__(&glReadPixels), ReadPixels)
#define DECLARE(type, name) static type name;
GL_PROCS(DECLARE)

static int init_gl(void)
{
    EGLDisplay d = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL);
    EGLConfig cfg;
    EGLint n, ca[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
    if (!eglInitialize(d, NULL, NULL) || !eglBindAPI(EGL_OPENGL_API) ||
        !eglChooseConfig(d, ca, &cfg, 1, &n) || !n) return 0;
    EGLint sa[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(d, cfg, sa);
    EGLContext ctx = eglCreateContext(d, cfg, EGL_NO_CONTEXT, NULL);
    if (!ctx || !surface || !eglMakeCurrent(d, surface, surface, ctx)) return 0;
#define LOAD(type, name) name = (type)eglGetProcAddress("gl" #name); if (!name) return 0;
    GL_PROCS(LOAD)
    const char *renderer = (const char *)GetString(GL_RENDERER);
    const char *extensions = (const char *)GetString(GL_EXTENSIONS);
    fprintf(stderr, "renderer=%s\n", renderer ? renderer : "?");
    return renderer && strstr(renderer,"D3D12") && extensions && strstr(extensions,"GL_EXT_memory_object_fd") && strstr(extensions,"GL_MESA_pack_invert");
}

