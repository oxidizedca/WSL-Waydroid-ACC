#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#define req(x) do{if(!(x)){fprintf(stderr,"FAILED %s line %d\n",#x,__LINE__);return 2;}}while(0)
#define EGLPROC(name) __typeof__(&name) f_##name=proc(#name);req(f_##name)
#define GLPROC(name) __typeof__(&name) f_##name=(__typeof__(&name))f_eglGetProcAddress(#name);req(f_##name)
int main(int argc,char **argv){
 req(argc==2);
 void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!lib)fprintf(stderr,"%s\n",dlerror());req(lib);
 int (*init)(void)=dlsym(lib,"glibc_ldr_init");EGLDisplay (*display)(void)=dlsym(lib,"glibc_ldr_display");
 void *(*proc)(const char*)=dlsym(lib,"glibc_ldr_proc");req(init&&display&&proc);req(!init());
 EGLDisplay d=display();req(d);EGLPROC(eglChooseConfig);EGLPROC(eglBindAPI);EGLPROC(eglCreateContext);EGLPROC(eglCreatePbufferSurface);EGLPROC(eglMakeCurrent);EGLPROC(eglGetProcAddress);
 EGLint ca[]={EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
 EGLConfig c=NULL; EGLint n=0; req(f_eglChooseConfig(d,ca,&c,1,&n)&&n==1);req(f_eglBindAPI(EGL_OPENGL_ES_API));
 EGLint ctxa[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE},sa[]={EGL_WIDTH,64,EGL_HEIGHT,64,EGL_NONE};
 EGLContext cx=f_eglCreateContext(d,c,EGL_NO_CONTEXT,ctxa);req(cx);
 EGLSurface s=f_eglCreatePbufferSurface(d,c,sa);req(s);req(f_eglMakeCurrent(d,s,s,cx));
 GLPROC(glGetString);GLPROC(glClearColor);GLPROC(glClear);GLPROC(glReadPixels);
 const char *r=(const char*)f_glGetString(GL_RENDERER);req(r&&strstr(r,"D3D12"));uint32_t pixels[4096];
 for(int i=0;i<3;i++){f_glClearColor(i==0,i==1,i==2,1);f_glClear(GL_COLOR_BUFFER_BIT);f_glReadPixels(0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
  for(int j=0;j<4096;j++)req(pixels[j]==(0xff000000u|(255u<<(i*8))));}
 fprintf(stderr,"PIXEL_OK renderer=%s\n",r);fflush(stderr);sleep(10);return 0;
}
