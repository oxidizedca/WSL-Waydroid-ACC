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

static int read_frame(int texture_fd, int w, int h, int stride, void *pixels, int invert)
{
    GLuint memory = 0, texture = 0, fbo = 0;
    GLint dedicated = GL_TRUE;
    CreateMemoryObjectsEXT(1, &memory);
    MemoryObjectParameterivEXT(memory, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
    /* Mesa takes ownership of the import fd. Dedicated resource at offset 0. */
    ImportMemoryFdEXT(memory, (GLuint64)w*h*4, GL_HANDLE_TYPE_OPAQUE_FD_EXT, texture_fd);
    GenTextures(1, &texture);
    BindTexture(GL_TEXTURE_2D, texture);
    TexStorageMem2DEXT(GL_TEXTURE_2D, 1, GL_RGBA8, w, h, memory, 0);
    GenFramebuffers(1, &fbo);
    BindFramebuffer(GL_FRAMEBUFFER, fbo);
    FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    int ok = CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE && GetError() == GL_NO_ERROR;
    if (ok) {
        PixelStorei(GL_PACK_ROW_LENGTH,stride);
        PixelStorei(GL_PACK_INVERT_MESA,invert);
        ReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        ok = GetError() == GL_NO_ERROR;
    }
    DeleteFramebuffers(1, &fbo);
    DeleteTextures(1, &texture);
    DeleteMemoryObjectsEXT(1, &memory);
    return ok;
}

static uid_t allowed[3];
static int sync_plane(int fd, uint64_t flags)
{
    struct dma_buf_sync sync = {.flags=flags};
    int rc;
    do { rc=ioctl(fd,DMA_BUF_IOCTL_SYNC,&sync); } while(rc<0 && errno==EINTR);
    return rc;
}

#include "publish.h"
#include "broker.h"

static void serve(int sock)
{
    struct ucred peer;
    socklen_t len=sizeof(peer);
    if (getsockopt(sock,SOL_SOCKET,SO_PEERCRED,&peer,&len) ||
        (peer.uid!=allowed[0] && peer.uid!=allowed[1] && peer.uid!=allowed[2])) return;
    struct timeval timeout={.tv_usec=300000};
    if (setsockopt(sock,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))) return;
    struct frame_frame peek;
    if (recv(sock,&peek,sizeof(peek),MSG_PEEK)==sizeof(peek) && peek.phase==WESTON_TEXTURE_FETCH) {
        if(peer.uid==1000) weston_texture_fetch(sock);
        return;
    }
    int fds[3];
    struct frame_frame p;
    if (!frame_packet(sock,&p,fds,3,0)) return;
    void *map=MAP_FAILED;
    size_t size=0;
    if (p.phase!=FRAME_REQUEST || !p.buffer_id || !p.width || !p.height ||
        p.width>4096 || p.height>4096 || p.stride<p.width || p.stride>8192) goto done;
    size=(size_t)p.stride*p.height*4;
    struct stat st;
    if (size>32*1024*1024 || fstat(fds[2],&st) || st.st_size<(off_t)size) goto done;
    struct pollfd ready={fds[1],POLLIN,0};
    if (poll(&ready,1,200)!=1 || !(ready.revents&POLLIN)) goto done;
    map=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_SHARED,fds[2],0);
    if (map==MAP_FAILED) goto done;
    p.phase=FRAME_READY;
    if (!frame_packet(sock,&p,NULL,0,1)) goto done;
    struct frame_frame grant;
    if (!frame_packet(sock,&grant,NULL,0,0) || grant.phase!=FRAME_WRITE ||
        grant.buffer_id!=p.buffer_id || grant.serial!=p.serial) goto done;
    /* After WRITE the producer keeps this slot until DONE or worker death.
     * A stuck GL call terminates this worker; systemd starts a fresh context. */
    alarm(3);
    if (sync_plane(fds[2],DMA_BUF_SYNC_START|DMA_BUF_SYNC_RW)) goto done;
    int candidate = peer.uid==1000 && p.width==1222 && frame_full_height(p.height) && p.stride>=1226;
    int texture = candidate ? fcntl(fds[0],F_DUPFD_CLOEXEC,3) : -1;
    int all_opaque=1, repaired=0;
    int ok=read_frame(fds[0],p.width,p.height,p.stride,map,1);
    fds[0]=-1;
    uint32_t nonzero=0;
    if (ok) {
        for (unsigned y=0;y<p.height;y++) {
            uint32_t *row=(uint32_t *)map+(size_t)y*p.stride;
            for (unsigned x=0;x<p.width;x++) {
                uint32_t v=row[x];
                if (p.alpha_fix && v && !(v>>24)) {row[x]=v|0xff000000u; repaired=1;}
                if ((row[x]>>24)!=255) all_opaque=0;
                nonzero|=v;
            }
        }
        p.sample=frame_sample(map,p.width,p.height,p.stride);
    }
    if (texture>=0) {
        if(ok && nonzero && (!repaired || all_opaque)) weston_texture_publish(&p,map,texture,all_opaque);
        close(texture);
    }
    if (sync_plane(fds[2],DMA_BUF_SYNC_END|DMA_BUF_SYNC_RW)) ok=0;
    if (ok) {
        p.phase=FRAME_DONE;
        p.nonempty=!!nonzero;
        frame_packet(sock,&p,NULL,0,1);
    }
done:
    alarm(0);
    if (map!=MAP_FAILED) munmap(map,size);
    for(int i=0;i<3;i++) if(fds[i]>=0) close(fds[i]);
}

int main(int argc,char **argv)
{
    struct sockaddr_un addr={.sun_family=AF_UNIX};
    if(argc==3 && !strcmp(argv[1],"--client-target"))
        return client_target_run(argv[2]);
    if (argc!=5 || strlen(argv[1])>=sizeof(addr.sun_path) || !init_gl()) return 1;
    for(int i=0;i<3;i++) {
        char *end;
        unsigned long uid=strtoul(argv[i+2],&end,10);
        if (*end || !uid || uid>UINT32_MAX) return 1;
        allowed[i]=uid;
    }
    strcpy(addr.sun_path,argv[1]);
    int server=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);
    if(server<0) return 1;
    umask(0077);
    unlink(addr.sun_path);
    if(bind(server,(struct sockaddr *)&addr,sizeof(addr))) return 1;
    /* Grant access only to the two nominated app UIDs plus the owner. */
    typedef void *(*from_text_fn)(const char *);
    void *lib=dlopen("libacl.so.1",RTLD_NOW);
    from_text_fn from_text=lib?dlsym(lib,"acl_from_text"):NULL;
    int (*set_file)(const char*,int,void*)=lib?dlsym(lib,"acl_set_file"):NULL;
    int (*release)(void*)=lib?dlsym(lib,"acl_free"):NULL;
    char spec[128];
    snprintf(spec,sizeof(spec),"u::rw,u:%u:rw,u:%u:rw,g::---,m::rw,o::---",allowed[1],allowed[2]);
    void *acl=from_text?from_text(spec):NULL;
    if(!acl || !set_file || !release || set_file(addr.sun_path,0x8000,acl)) {perror("socket ACL");return 1;}
    release(acl);
    if(listen(server,12)) return 1;
    for(;;) {
        int sock=accept4(server,NULL,NULL,SOCK_CLOEXEC);
        if(sock<0) {if(errno==EINTR)continue;return 1;}
        serve(sock);
        close(sock);
    }
}

