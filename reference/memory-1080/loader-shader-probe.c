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
 GLPROC(glCreateShader);GLPROC(glShaderSource);GLPROC(glCompileShader);GLPROC(glGetShaderiv);
 GLPROC(glCreateProgram);GLPROC(glAttachShader);GLPROC(glLinkProgram);GLPROC(glGetProgramiv);GLPROC(glUseProgram);
 GLPROC(glBindAttribLocation);GLPROC(glVertexAttribPointer);GLPROC(glEnableVertexAttribArray);GLPROC(glDrawArrays);GLPROC(glViewport);
 GLPROC(glGetError);
 const char *source[2]={"attribute vec2 pos;void main(){gl_Position=vec4(pos,0.,1.);}","precision mediump float;void main(){gl_FragColor=vec4(1.,0.,1.,1.);}"};
 GLuint prog=f_glCreateProgram(); GLint ok;req(prog);
 for(int i=0;i<2;i++){GLuint sh=f_glCreateShader(i?GL_FRAGMENT_SHADER:GL_VERTEX_SHADER);req(sh);f_glShaderSource(sh,1,&source[i],NULL);f_glCompileShader(sh);f_glGetShaderiv(sh,GL_COMPILE_STATUS,&ok);req(ok);f_glAttachShader(prog,sh);}
 f_glBindAttribLocation(prog,0,"pos");f_glLinkProgram(prog);f_glGetProgramiv(prog,GL_LINK_STATUS,&ok);req(ok);f_glUseProgram(prog);
 const GLfloat pos[]={-1,-1,3,-1,-1,3};f_glViewport(0,0,64,64);f_glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,pos);f_glEnableVertexAttribArray(0);f_glDrawArrays(GL_TRIANGLES,0,3);
 f_glReadPixels(0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,pixels);req(f_glGetError()==GL_NO_ERROR);
 for(int j=0;j<4096;j++)req(pixels[j]==0xffff00ffu);
 fprintf(stderr,"SHADER_PIXEL_OK renderer=%s\n",r);return 0;
}
