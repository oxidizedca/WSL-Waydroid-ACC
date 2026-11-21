#include "gl-init.h"
#include "gpu.h"
#include <assert.h>

int main(void)
{
    assert(init_gl() && init_input());
    int (*snapshot)(int,int,int *,void **,int,int64_t *)=dlsym(RTLD_DEFAULT,"texture_cache_export_target");
    int (*read_copy)(void *,void *,unsigned)=dlsym(RTLD_DEFAULT,"physical_snapshot_read");
    void (*drop)(void *)=dlsym(RTLD_DEFAULT,"physical_snapshot_drop");
    int (*bind)(int,unsigned,int,int,void *,uint64_t,uint64_t,unsigned,int *)=dlsym(RTLD_DEFAULT,"texture_cache_bind_input");
    void (*forget)(void *)=dlsym(RTLD_DEFAULT,"texture_cache_forget_image");
    __typeof__(&glClearColor) color=(void *)eglGetProcAddress("glClearColor");
    assert(snapshot && read_copy && drop && bind && forget && color);
    GLuint src, sample;
    GenTextures(1,&src); GenTextures(1,&sample);
    BindFramebuffer(GL_FRAMEBUFFER,fbo);
    void *held[12]={0};
    int waits[12],textures[12],hits=0;
    puts("frame,enabled,create_ns,submit_ns,handle_ns,spare");
    for(int i=0;i<12;i++) {
        int w=i<9?1920:640,h=i<6?1080:(i<9?1048:480);
        BindTexture(GL_TEXTURE_2D,src);
        TexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
        FramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,src,0);
        assert(CheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);
        color((i+1)/255.f,0,1,1); Clear(GL_COLOR_BUFFER_BIT);
        int fds[2]; int64_t t[4]={0};
        assert(!snapshot(w,h,fds,&held[i],i>=3,t));
        textures[i]=fds[0]; waits[i]=fds[1];
        hits+=t[3];
        printf("%d,%d,%lld,%lld,%lld,%d\n",i,i>=3,(long long)t[0],(long long)t[1],(long long)t[2],(int)t[3]);
        usleep(8000);
    }
    assert(hits>0);
    for(int i=11;i>=0;i--) {
        int w=i<9?1920:640,h=i<6?1080:(i<9?1048:480);
        struct pollfd pf={.fd=waits[i],.events=POLLIN};
        assert(poll(&pf,1,2000)==1 && pf.revents&POLLIN);
        uint32_t *pixels=malloc((size_t)w*h*4);
        assert(read_copy(held[i],pixels,w));
        uint32_t expected=0xffff0000u+(unsigned)i+1;
        for(int j=0;j<w*h;j++) assert(pixels[j]==expected);
        free(pixels);
        int hit;
        for(int j=0;j<3;j++) {
            if(j==2) forget(held);
            BindTexture(GL_TEXTURE_2D,sample);
            assert(bind(textures[i],GL_TEXTURE_2D,w,h,held,1,i+1,i,&hit) && hit==(j==1));
            FramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,sample,0);
            uint32_t rgba=0;
            ReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,&rgba);
            assert(rgba==expected);
        }
        drop(held[i]); close(textures[i]); close(waits[i]);
    }
    forget(held);
    EGLDisplay d=eglGetCurrentDisplay(); EGLContext ctx=eglGetCurrentContext();
    eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
    assert(eglDestroyContext(d,ctx)); eglTerminate(d);
}
