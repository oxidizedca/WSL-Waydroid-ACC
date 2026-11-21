#include "gl-init.h"
#include "state.h"
#include "gpu.h"
#include "visibility.h"
static int can_check_visibility;
static int server=-1, deferred=-1;
static unsigned queries, interleaved, postponed;
static const struct entry *publishing;
static void service_ready_get(void);
#include "mirror-digest.h"
#include <sys/random.h>
struct entry {
    struct frame_frame p;
    struct input_stamp stamp;
    struct stat plane_stat;
    int plane,texture,lazy,retained,role;
    uid_t uid;pid_t pid;int sampled;
    unsigned index;
    GLuint tex;
    void *map;
    size_t size;
    unsigned char digest[32];
};
static struct entry entries[INPUT_SLOTS];
static unsigned next_entry;
#include "packages.h"
static volatile sig_atomic_t stopping;
static void stop(int sig) {stopping=1;}
static int sync_buf(int fd,uint64_t flags)
{
    struct dma_buf_sync sync={.flags=flags};
    int rc;
    do {rc=ioctl(fd,DMA_BUF_IOCTL_SYNC,&sync);} while(rc<0 && errno==EINTR);
    return rc==0;
}
static void commit_stamp(struct entry *e)
{
    set_generation(e->index,(struct generation){
        e->p.buffer_id,e->plane_stat.st_dev,e->plane_stat.st_ino,e->stamp.nonce,e->stamp.serial,e->stamp.magic});
}
static int mirror(struct entry *e)
{
    if(!e->lazy) return 1;
    if(!sync_buf(e->plane,DMA_BUF_SYNC_START|DMA_BUF_SYNC_WRITE)) return 0;
    BindFramebuffer(GL_FRAMEBUFFER,fbo);
    FramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,e->tex,0);
    PixelStorei(GL_PACK_ROW_LENGTH,e->p.stride);
    int ok=CheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE;
    if(ok) ReadPixels(0,0,e->p.width,e->p.height,GL_RGBA,GL_UNSIGNED_BYTE,e->map);
    ok=ok && GetError()==GL_NO_ERROR;
    if(ok) {
        e->stamp.magic=LAYER_INPUT_STAMP;
        mirror_digest(e->map,e->size,e->digest);
    }
    if(!sync_buf(e->plane,DMA_BUF_SYNC_END|DMA_BUF_SYNC_WRITE)) ok=0;
    if(ok) {commit_stamp(e);e->lazy=0;}
    return ok;
}
#include "retain.h"
static void release_entry(struct entry *e)
{
    if(!e->tex) return;
    if(e->retained) forget_generation(e->stamp.nonce);
    DeleteTextures(1,&e->tex);
    close(e->texture);close(e->plane);munmap(e->map,e->size);
    memset(e,0,sizeof(*e));
}
static struct entry *find_entry(uint64_t id,const struct stat *st)
{
    for(unsigned i=0;i<INPUT_SLOTS;i++)
        if(entries[i].tex && entries[i].p.buffer_id==id && entries[i].plane_stat.st_dev==st->st_dev &&
           entries[i].plane_stat.st_ino==st->st_ino) return &entries[i];
    return NULL;
}
static int window_confirmed(const struct frame_frame *p,const struct ucred *peer)
{
    for(unsigned i=0;i<INPUT_SLOTS;i++) {
        const struct entry *e=&entries[i];
        if(e->sampled && e->uid==peer->uid && e->pid==peer->pid &&
           (e->p.alpha_fix>>2)==(p->alpha_fix>>2) && e->p.width==p->width &&
           e->p.height==p->height) return 1;
    }
    return 0;
}
static int fd_kind(int fd,const char *kind,int prefix)
{
    char path[64],name[256];
    snprintf(path,sizeof(path),"/proc/self/fd/%d",fd);
    ssize_t n=readlink(path,name,sizeof(name)-1);
    if(n<0) return 0;
    name[n]=0;
    return prefix ? !strncmp(name,kind,strlen(kind)) : !strcmp(name,kind);
}
static int publish(struct frame_frame *p,int fd[3],int role,const struct ucred *peer,const char *name,int *confirmed_empty)
{
    if(!(p->alpha_fix>>2) || !accepts_shape(role,p->width,p->height) ||
       !input_layout(p->width,p->height,p->stride) || !p->buffer_id ||
       !fd_kind(fd[0],"anon_inode:dxgresource",0) || !fd_kind(fd[1],"anon_inode:[eventfd]",0) ||
       !fd_kind(fd[2],"/dmabuf:",1)) return 0;
    struct entry e={.p=*p,.role=role,.uid=peer->uid,.pid=peer->pid,.sampled=window_confirmed(p,peer),
                    .size=(size_t)p->stride*p->height*4,.lazy=1};
    struct pollfd fence={.fd=fd[1],.events=POLLIN};
    if(fstat(fd[2],&e.plane_stat) || e.plane_stat.st_size<(off_t)e.size ||
       poll(&fence,1,700)!=1 || !(fence.revents&POLLIN)) return 0;
    struct entry *slot=find_entry(p->buffer_id,&e.plane_stat);
    if(slot && slot->uid!=peer->uid) return 0;
    if(getrandom(&e.stamp.nonce,8,0)!=8 || !e.stamp.nonce) return 0;
    e.stamp.serial=p->serial;
    e.stamp.magic=LAYER_INPUT_STAMP+1;
    e.map=mmap(NULL,e.size,PROT_READ|PROT_WRITE,MAP_SHARED,fd[2],0);
    if(e.map==MAP_FAILED) return 0;
    /* A sampled app window may intentionally clear every pixel on exit. */
    int allow_empty = (p->alpha_fix & 2) &&
        ((role == SYSTEMUI && frame_full_height(p->height)) || (role == ORDINARY && e.sampled));
    /* A producer can only skip the empty-frame query after this broker has
     * confirmed a generation from the same window, process and dimensions. */
    int fused = p->phase==APP_WINDOW_WINDOW;
    int system_window = p->phase==SYSTEM_WINDOW_SYSTEM_WINDOW;
    if(system_window && (role>=ORDINARY || !can_check_visibility || !e.sampled || !p->nonempty)) {munmap(e.map,e.size);return 0;}
    if(fused && (role!=ORDINARY || !allow_empty || !p->nonempty)) {munmap(e.map,e.size);return 0;}
    int dupfd=fcntl(fd[0],F_DUPFD_CLOEXEC,3);
    if(dupfd>=0)
        e.tex=(fused || system_window) ? import_texture(dupfd,p->width,p->height) :
                      normalized(dupfd,p->alpha_fix&1,p->width,p->height,allow_empty,
                                 role==LAUNCHER && p->width==1920 && frame_full_height(p->height) &&
                                 !e.sampled && !(p->alpha_fix&2) ? confirmed_empty : NULL);
    if(!e.tex) {munmap(e.map,e.size);return 0;}
    if(system_window && !allow_empty) {
        publishing=&e;
        if(!access("/run/waydroid-acc-query-on",F_OK)) visibility_pending=service_ready_get;
        int visible=texture_has_color(e.tex,p->width,p->height);
        visibility_pending=NULL;
        publishing=NULL;
        if(!visible) {DeleteTextures(1,&e.tex);munmap(e.map,e.size);return 0;}
    }
    e.texture=fd[0]; e.plane=fd[2]; fd[0]=fd[2]=-1;
    if(!slot) {
        slot=&entries[next_entry++%INPUT_SLOTS];
        if(slot->tex && !mirror(slot)) {release_entry(&e);return 0;}
    }
    e.index=(unsigned)(slot-entries);
    int ok;
    if(p->nonempty && e.sampled && (e.retained=retain_generation(&e))) {
        commit_stamp(&e);
        ok=1;
    } else {p->nonempty=0;ok=mirror(&e);}
    if(!ok) {release_entry(&e);return 0;}
    release_entry(slot);
    *slot=e;
    return 1;
}
static void serve_input(int sd)
{
    struct ucred peer; socklen_t len=sizeof(peer);
    struct frame_frame p;
    if(getsockopt(sd,SOL_SOCKET,SO_PEERCRED,&peer,&len) ||
       recv(sd,&p,sizeof(p),MSG_PEEK)!=sizeof(p)) return;
    char name[256]={0};
    int pub=(p.phase==LAYER_INPUT_PUBLISH || p.phase==APP_WINDOW_WINDOW || p.phase==SYSTEM_WINDOW_SYSTEM_WINDOW),role=peer_role(peer.pid,peer.uid,name);
    if(pub ? (role<0 || role>=PRODUCERS) : (role!=PRODUCERS && !(role==PRODUCERS+1 && p.phase==LAYER_INPUT_CPU))) return;
    int fd[3]={-1,-1,-1},out=-1;
    if(!frame_packet(sd,&p,fd,pub?3:1,0)) return;
    if(pub) {
        int confirmed_empty=0;
        int accepted=publish(&p,fd,role,&peer,name,&confirmed_empty);
        p.phase=accepted ? LAYER_INPUT_OK : (confirmed_empty ? EMPTY_FRAME_EMPTY : LAYER_INPUT_REJECT);
        p.sample=0;
        if(p.phase==LAYER_INPUT_OK && window_confirmed(&p,&peer))
            p.sample=LAYER_INPUT_SAMPLED | (role==ORDINARY ? APP_WINDOW_CAN_IMPORT : (can_check_visibility ? SYSTEM_WINDOW_CAN_CHECK : 0));
    }
    else {
        struct stat st;
        struct entry *e=fstat(fd[0],&st)?NULL:find_entry(p.buffer_id,&st);
        uint64_t nonce=((uint64_t)p.alpha_fix<<32)|p.sample;
        int phase=p.phase;
        p.phase=LAYER_INPUT_MISS;
        if(e && e->stamp.nonce==nonce && e->stamp.serial==p.serial) {
            if(phase==LAYER_INPUT_BOUND) {p.phase=LAYER_INPUT_OK;e->sampled=1;}
            if(phase==LAYER_INPUT_CPU) p.phase=mirror(e)?LAYER_INPUT_OK:LAYER_INPUT_REJECT;
            if(phase==LAYER_INPUT_GET) {
                int valid=e->lazy;
                if(!valid && sync_buf(e->plane,DMA_BUF_SYNC_START|DMA_BUF_SYNC_READ)) {
                    unsigned char hash[32];
                    mirror_digest(e->map,e->size,hash);
                    valid=sync_buf(e->plane,DMA_BUF_SYNC_END|DMA_BUF_SYNC_READ) && !memcmp(hash,e->digest,32);
                }
                if(valid) {p.width=e->p.width;p.height=e->p.height;p.stride=e->p.stride;out=e->texture;p.phase=LAYER_INPUT_OK;p.nonempty=e->lazy;}
            }
        }
    }
    frame_packet(sd,&p,&out,out>=0,1);
    for(unsigned i=0;i<3;i++) if(fd[i]>=0) close(fd[i]);
}
/* Only a committed lazy GET can run between EndQuery and its result.
 * Keep the first other packet at the head of the normal dispatch queue. */
static void service_ready_get(void)
{
    ++queries;
    for(unsigned i=0;i<4 && deferred<0 && !stopping;++i) {
        GLuint ready=0;
        GetQueryObjectuiv(query,GL_QUERY_RESULT_AVAILABLE,&ready);
        if(ready) break;
        struct pollfd pf={.fd=server,.events=POLLIN};
        struct timespec slice={.tv_nsec=100000};
        if(ppoll(&pf,1,&slice,NULL)<=0) continue;
        int sd=accept4(server,NULL,NULL,SOCK_CLOEXEC|SOCK_NONBLOCK);
        if(sd<0) break;
        struct frame_frame p;
        int eligible=0;
        if(recv(sd,&p,sizeof(p),MSG_PEEK|MSG_DONTWAIT)==sizeof(p) &&
           p.magic==FRAME_MAGIC && p.phase==LAYER_INPUT_GET && p.buffer_id!=publishing->p.buffer_id) {
            uint64_t nonce=((uint64_t)p.alpha_fix<<32)|p.sample;
            for(unsigned j=0;j<INPUT_SLOTS;++j) {
                const struct entry *e=&entries[j];
                if(e->lazy && e->p.buffer_id==p.buffer_id && e->stamp.nonce==nonce &&
                   e->stamp.serial==p.serial && (e->pid!=publishing->pid ||
                   (e->p.alpha_fix>>2)!=(publishing->p.alpha_fix>>2))) {eligible=1;break;}
            }
        }
        if(!eligible) {deferred=sd;++postponed;break;}
        serve_input(sd);
        ++interleaved;
        close(sd);
    }
    if(queries%32==0) fprintf(stderr,"texture-cache queries=%u interleaved=%u deferred=%u\n",queries,interleaved,postponed);
}
int main(int argc,char **argv)
{
    char path[128];
    if(argc!=3 || strlen(argv[1])>=sizeof(((struct sockaddr_un*)0)->sun_path)) return 1;
    socket_path=argv[1];package_path=argv[2];
    snprintf(path,sizeof(path),"%s.state",argv[1]);
    if(!open_table(path,1) || !init_gl() || !init_input() || !restore_generations()) return 1;
    can_check_visibility=init_visibility();
    fprintf(stderr,"system-window system window visibility=%d\n",can_check_visibility);
    for(unsigned i=0;i<INPUT_SLOTS;i++) set_generation(i,(struct generation){0});
    struct sigaction sa={.sa_handler=stop};
    sigaction(SIGTERM,&sa,NULL);sigaction(SIGINT,&sa,NULL);
    struct sockaddr_un addr={.sun_family=AF_UNIX};
    strcpy(addr.sun_path,argv[1]);
    server=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
    umask(0077);unlink(addr.sun_path);
    if(bind(server,(void*)&addr,sizeof(addr)) || listen(server,16)) return 1;
    if(!refresh_packages()) return 1;
    while(!stopping) {
        refresh_packages();
        struct pollfd pf={.fd=server,.events=POLLIN};
        int sd=deferred;
        if(sd>=0) {
            deferred=-1;
            if(fcntl(sd,F_SETFL,0)<0) {close(sd); continue;}
        } else {
            if(poll(&pf,1,500)<=0) continue;
            sd=accept4(server,NULL,NULL,SOCK_CLOEXEC);
        }
        if(sd>=0) {
            struct timeval t={.tv_sec=2};
            setsockopt(sd,SOL_SOCKET,SO_RCVTIMEO,&t,sizeof(t));
            setsockopt(sd,SOL_SOCKET,SO_SNDTIMEO,&t,sizeof(t));
            serve_input(sd);close(sd);
        }
    }
    if(deferred>=0) close(deferred);
    int clean=1;
    for(unsigned i=0;i<INPUT_SLOTS;i++) if(entries[i].tex) {
        if(mirror(&entries[i])) release_entry(&entries[i]); else clean=0;
    }
    close(server);unlink(addr.sun_path);
    return clean ? 0 : 1;
}
