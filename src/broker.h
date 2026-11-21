#include <sys/random.h>
#define CLIENT_TARGET_PUBLISH 7
#define CLIENT_TARGET_LOOKUP 8
#define CLIENT_TARGET_BYTES (1920u*1048u*4u)
struct client_snapshot {
    uint64_t id, nonce;
    uint32_t serial;
    int texture, ready, qualified;
    unsigned char digest[32];
};
static struct client_snapshot client_frames[8];
static unsigned client_next;

/* SO_PEERCRED supplies the WSL pid of the connecting Android process. */
static int client_target_peer(pid_t pid,const char *expected)
{
    char path[64],comm[64]={0};
    snprintf(path,sizeof(path),"/proc/%d/comm",pid);
    FILE *f=fopen(path,"r");
    if(!f) return 0;
    int ok=fgets(comm,sizeof(comm),f)!=NULL;
    fclose(f);
    comm[strcspn(comm,"\n")]=0;
    return ok && !strcmp(comm,expected);
}
static void client_target_store(int sock)
{
    struct frame_frame p;
    int fds[2];
    if(!frame_packet(sock,&p,fds,2,0)) return;
    uint64_t nonce=0;
    unsigned char digest[32];
    if(p.phase!=CLIENT_TARGET_PUBLISH || !p.buffer_id || p.width!=1920 || p.height!=1048 ||
       p.stride!=1920 || recv(sock,digest,32,MSG_TRUNC)!=32 ||
       getrandom(&nonce,sizeof(nonce),0)!=sizeof(nonce) || !nonce) goto done;
    struct client_snapshot *e=NULL;
    for(unsigned i=0;i<8;i++) if(client_frames[i].id==p.buffer_id) e=&client_frames[i];
    if(!e) e=&client_frames[client_next++%8];
    int audit=e->id!=p.buffer_id || !e->qualified || p.serial%60==0;
    if(e->id) {
        close(e->texture);
        close(e->ready);
    }
    *e=(struct client_snapshot){.id=p.buffer_id,.serial=p.serial,.nonce=nonce,
        .texture=fds[0],.ready=fds[1],.qualified=!audit};
    memcpy(e->digest,digest,32);
    fds[0]=fds[1]=-1;
done:
    for(int i=0;i<2;i++) if(fds[i]>=0) close(fds[i]);
}
static void client_target_lookup(int sock)
{
    struct frame_frame p;
    int plane;
    if(!frame_packet(sock,&p,&plane,1,0)) return;
    for(unsigned i=0;i<8;i++) {
        struct client_snapshot *e=&client_frames[i];
        if(!e->id || e->id!=p.buffer_id || p.phase!=CLIENT_TARGET_LOOKUP) continue;
        struct pollfd ready={.fd=e->ready,.events=POLLIN};
        if(poll(&ready,1,0)!=1 || !(ready.revents&POLLIN)) break;
        struct stat st;
        if(fstat(plane,&st) || st.st_size<CLIENT_TARGET_BYTES || sync_plane(plane,DMA_BUF_SYNC_START|DMA_BUF_SYNC_READ)) break;
        void *map=mmap(NULL,CLIENT_TARGET_BYTES,PROT_READ,MAP_SHARED,plane,0);
        int ok=map!=MAP_FAILED;
        unsigned char digest[32];
        if(ok) {
            ok=SHA256(map,CLIENT_TARGET_BYTES,digest) && !memcmp(digest,e->digest,32);
            munmap(map,CLIENT_TARGET_BYTES);
        }
        if(sync_plane(plane,DMA_BUF_SYNC_END|DMA_BUF_SYNC_READ)) ok=0;
        if(ok && !e->qualified) {
            void *image=malloc(CLIENT_TARGET_BYTES);
            int copy=image ? fcntl(e->texture,F_DUPFD_CLOEXEC,3) : -1;
            ok=copy>=0 && read_frame(copy,1920,1048,1920,image,0) &&
                SHA256(image,CLIENT_TARGET_BYTES,digest) && !memcmp(digest,e->digest,32);
            free(image);
            e->qualified=ok;
            fprintf(stderr,"client-target audit id=%llu serial=%u match=%d\n",(unsigned long long)e->id,e->serial,ok);
        }
        if(ok) {
            p.phase=FRAME_DONE;
            p.serial=e->serial;
            p.alpha_fix=e->nonce>>32;
            p.sample=(uint32_t)e->nonce;
            frame_packet(sock,&p,&e->texture,1,1);
        }
        break;
    }
    close(plane);
}

/* A producer waiting to dequeue an app buffer on the t2 socket must never
 * serialize the HWC lookup needed to finish that same Android present. */
static int client_target_run(const char *path)
{
    struct sockaddr_un addr={.sun_family=AF_UNIX};
    if(strlen(path)>=sizeof(addr.sun_path) || !init_gl()) return 1;
    strcpy(addr.sun_path,path);
    int listener=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);
    if(listener<0) return 1;
    umask(0077);
    unlink(path);
    if(bind(listener,(void *)&addr,sizeof(addr)) || listen(listener,12)) return 1;
    for(;;) {
        int sock=accept4(listener,NULL,NULL,SOCK_CLOEXEC);
        if(sock<0) {if(errno==EINTR) continue; return 1;}
        struct ucred peer;
        socklen_t size=sizeof(peer);
        struct timeval limit={.tv_usec=25000};
        struct frame_frame msg;
        if(!getsockopt(sock,SOL_SOCKET,SO_PEERCRED,&peer,&size) && peer.uid==getuid() &&
           !setsockopt(sock,SOL_SOCKET,SO_RCVTIMEO,&limit,sizeof(limit)) &&
           recv(sock,&msg,sizeof(msg),MSG_PEEK)==sizeof(msg) && msg.magic==FRAME_MAGIC) {
            if(msg.phase==CLIENT_TARGET_PUBLISH && client_target_peer(peer.pid,"surfaceflinger")) client_target_store(sock);
            else if(msg.phase==CLIENT_TARGET_LOOKUP && client_target_peer(peer.pid,"composer@2.1-se")) client_target_lookup(sock);
        }
        close(sock);
    }
}
