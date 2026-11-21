#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#define CK(x)                                                                  \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "%s:%d %s EGL=%x GL=%x\n", role, __LINE__, #x,           \
              eglGetError(), glGetError());                                    \
      return 2;                                                                \
    }                                                                          \
  } while (0)
static int work(int sock, const char *role, int writer, uint64_t usage) {
  AHardwareBuffer *b = NULL;
  if (writer) {
    AHardwareBuffer_Desc desc = {.width = 1920,
                                 .height = 1080,
                                 .layers = 1,
                                 .format =
                                     AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
                                 .usage = usage};
    CK(!AHardwareBuffer_allocate(&desc, &b));
  } else {
    CK(!AHardwareBuffer_recvHandleFromUnixSocket(sock, &b));
  }
  AHardwareBuffer_Desc actual;
  AHardwareBuffer_describe(b, &actual);
  printf("%s usage=%llx size=%ux%u stride=%u format=%d\n", role,
         (unsigned long long)actual.usage, actual.width, actual.height,
         actual.stride, actual.format);
  EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  CK(eglInitialize(d, NULL, NULL));
  EGLint attrs[] = {EGL_RENDERABLE_TYPE,
                    EGL_OPENGL_ES2_BIT,
                    EGL_SURFACE_TYPE,
                    EGL_PBUFFER_BIT,
                    EGL_RED_SIZE,
                    8,
                    EGL_GREEN_SIZE,
                    8,
                    EGL_BLUE_SIZE,
                    8,
                    EGL_NONE};
  EGLConfig cfg;
  EGLint n;
  CK(eglChooseConfig(d, attrs, &cfg, 1, &n) && n);
  EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE},
         sa[] = {EGL_WIDTH, 4, EGL_HEIGHT, 4, EGL_NONE};
  EGLContext ctx = eglCreateContext(d, cfg, EGL_NO_CONTEXT, ca);
  EGLSurface surf = eglCreatePbufferSurface(d, cfg, sa);
  CK(ctx && surf && eglMakeCurrent(d, surf, surf, ctx));
  printf("%s renderer=%s\n", role, glGetString(GL_RENDERER));
  PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC native =
      (void *)eglGetProcAddress("eglGetNativeClientBufferANDROID");
  PFNEGLCREATEIMAGEKHRPROC create =
      (void *)eglGetProcAddress("eglCreateImageKHR");
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image =
      (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");
  CK(native && create && image);
  EGLint ia[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
  EGLImageKHR im =
      create(d, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, native(b), ia);
  CK(im);
  GLuint tex, fb;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  image(GL_TEXTURE_2D, im);
  CK(!glGetError());
  glGenFramebuffers(1, &fb);
  glBindFramebuffer(GL_FRAMEBUFFER, fb);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         tex, 0);
  CK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
  if (writer) {
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    glScissor(1860, 500, 20, 80);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    glFlush();
    glFinish();
  }
  uint8_t bg[4] = {0}, bar[4] = {0};
  glReadPixels(40, 540, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, bg);
  glReadPixels(1870, 540, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, bar);
  CK(!glGetError());
  printf("%s bg=%02x%02x%02x%02x bar=%02x%02x%02x%02x\n", role, bg[0], bg[1],
         bg[2], bg[3], bar[0], bar[1], bar[2], bar[3]);
  if (writer) {
    CK(!AHardwareBuffer_sendHandleToUnixSocket(b, sock));
    char ack;
    CK(read(sock, &ack, 1) == 1);
  } else {
    CK(write(sock, "!", 1) == 1);
  }
  return memcmp(bg, (uint8_t[]){0, 0, 255, 255}, 4) ||
                 memcmp(bar, (uint8_t[]){255, 0, 0, 255}, 4)
             ? 3
             : 0;
}
int main(int argc, char **argv) {
  setbuf(stdout, NULL);
  uint64_t u = argc == 2 ? strtoull(argv[1], NULL, 0) : 0xb00;
  int s[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, s))
    return 1;
  pid_t p = fork();
  if (p < 0)
    return 1;
  if (!p) {
    close(s[0]);
    _exit(work(s[1], "reader", 0, u));
  }
  close(s[1]);
  int rc = work(s[0], "writer", 1, u);
  close(s[0]);
  int status;
  waitpid(p, &status, 0);
  return rc ? rc : WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
