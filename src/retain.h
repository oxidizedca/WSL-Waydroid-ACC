/* Each stored socket contains one unread SCM_RIGHTS message with the exact
 * texture, plane and generation. MSG_PEEK permits retry after another restart. */
extern int sd_pid_notify_with_fds(pid_t,int,const char *,const int *,unsigned);
extern int sd_notify(int,const char *);
extern int sd_notify_barrier(int,uint64_t);
extern int sd_listen_fds_with_names(int,char ***);

static void forget_generation(uint64_t nonce)
{
    char msg[96];
    snprintf(msg,sizeof(msg),"FDSTOREREMOVE=1\nFDNAME=texture-%016llx",(unsigned long long)nonce);
    sd_notify(0,msg);
}
static int retain_generation(struct entry *e)
{
    int pair[2];
    if(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,pair))return 0;
    char msg[96];
    snprintf(msg,sizeof(msg),"FDSTORE=1\nFDPOLL=0\nFDNAME=texture-%016llx",(unsigned long long)e->stamp.nonce);
    struct frame_frame p=e->p;
    p.nonempty=e->index;
    p.sample=e->stamp.nonce;
    p.alpha_fix=e->stamp.nonce>>32;
    int rights[]={e->texture,e->plane};
    int ok=frame_packet(pair[0],&p,rights,2,1) &&
           sd_pid_notify_with_fds(0,0,msg,&pair[1],1)>0 && sd_notify_barrier(0,500000)>0;
    close(pair[0]);close(pair[1]);
    if(!ok) forget_generation(e->stamp.nonce);
    return ok;
}

static int restore_generation(int sd)
{
    struct frame_frame p;
    struct iovec v={&p,sizeof(p)};
    union {struct cmsghdr align; char bytes[CMSG_SPACE(2*sizeof(int))];} control={0};
    struct msghdr msg={.msg_iov=&v,.msg_iovlen=1,.msg_control=control.bytes,.msg_controllen=sizeof(control)};
    if(recvmsg(sd,&msg,MSG_PEEK|MSG_CMSG_CLOEXEC)!=sizeof(p)) return 0;
    struct cmsghdr *c=CMSG_FIRSTHDR(&msg);
    if(!c || c->cmsg_level!=SOL_SOCKET || c->cmsg_type!=SCM_RIGHTS || c->cmsg_len!=CMSG_LEN(2*sizeof(int)))return 0;
    int fd[2];memcpy(fd,CMSG_DATA(c),sizeof(fd));
    int ok=0;
    struct entry e={.p=p,.index=p.nonempty,.plane=fd[1],.texture=fd[0],.lazy=1,
                    .size=(size_t)p.stride*p.height*4};
    e.stamp=(struct input_stamp){.nonce=((uint64_t)p.alpha_fix<<32)|p.sample,.serial=p.serial,.magic=LAYER_INPUT_STAMP+1};
    if(p.magic==FRAME_MAGIC && e.index<INPUT_SLOTS && input_layout(p.width,p.height,p.stride) &&
       !fstat(e.plane,&e.plane_stat) && e.plane_stat.st_size>=(off_t)e.size) {
        e.map=mmap(NULL,e.size,PROT_READ|PROT_WRITE,MAP_SHARED,e.plane,0);
        if(e.map!=MAP_FAILED) {
            struct input_stamp mark=lookup_stamp(p.buffer_id,&e.plane_stat);
            if(mark.magic!=LAYER_INPUT_STAMP+1 || mark.serial!=p.serial || mark.nonce!=e.stamp.nonce) ok=1;
            else {
                int texfd=fcntl(e.texture,F_DUPFD_CLOEXEC,3);
                if(texfd>=0 && (e.tex=import_texture(texfd,p.width,p.height))) {
                    ok=mirror(&e);
                    DeleteTextures(1,&e.tex);
                    if(ok) fprintf(stderr,"layer-input recovered id=%llu seq=%u\n",(unsigned long long)p.buffer_id,p.serial);
                }
            }
            munmap(e.map,e.size);
        }
    }
    close(fd[0]);close(fd[1]);
    if(ok) forget_generation(e.stamp.nonce);
    return ok;
}
static int restore_generations(void)
{
    char **names=NULL;
    int n=sd_listen_fds_with_names(0,&names),ok=n>=0;
    for(int i=0;i<n;i++) {
        if(strncmp(names[i],"texture-",sizeof("texture-")-1) || !restore_generation(3+i)) ok=0;
        free(names[i]);close(3+i);
    }
    free(names);
    return ok;
}
