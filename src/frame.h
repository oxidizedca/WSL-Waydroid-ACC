#pragma once
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Full-height layouts in the legacy panel and panel-free nested desktop. */
static inline int frame_full_height(unsigned h) { return h == 1048 || h == 1080; }

#define FRAME_SOCKET "/data/local/tmp/waydroid-acc/frames"
#define FRAME_MAGIC 0x54324451u
enum frame_phase { FRAME_REQUEST, FRAME_READY, FRAME_WRITE, FRAME_DONE };
struct frame_frame {
    uint32_t magic, phase, width, height, stride, serial, alpha_fix, nonempty;
    uint64_t buffer_id;
    uint32_t sample;
};

static inline int frame_packet(int sock, struct frame_frame *p, int *fd, int count, int sending)
{
    struct iovec io = {p, sizeof(*p)};
    union { struct cmsghdr align; char data[CMSG_SPACE(3*sizeof(int))]; } control = {0};
    struct msghdr msg = {.msg_iov=&io,.msg_iovlen=1};
    if (!sending || count) {
        msg.msg_control = control.data;
        msg.msg_controllen = sending ? CMSG_SPACE(count*sizeof(int)) : sizeof(control);
    }
    if (sending && count) {
        struct cmsghdr *c=CMSG_FIRSTHDR(&msg);
        c->cmsg_level=SOL_SOCKET; c->cmsg_type=SCM_RIGHTS;
        c->cmsg_len=CMSG_LEN(count*sizeof(int));
        memcpy(CMSG_DATA(c),fd,count*sizeof(int));
    }
    ssize_t n;
    do {
        n = sending ? sendmsg(sock,&msg,MSG_NOSIGNAL) : recvmsg(sock,&msg,MSG_CMSG_CLOEXEC);
    } while (n<0 && errno==EINTR);
    if (sending) return n==sizeof(*p);
    if (n<=0) return 0;
    int received=0;
    for (struct cmsghdr *c=CMSG_FIRSTHDR(&msg); c; c=CMSG_NXTHDR(&msg,c)) {
        if (c->cmsg_type!=SCM_RIGHTS || c->cmsg_level!=SOL_SOCKET) continue;
        size_t nf=(c->cmsg_len-CMSG_LEN(0))/sizeof(int);
        int *v=(int *)CMSG_DATA(c);
        for (size_t i=0;i<nf;i++,received++) {
            if (received<count) fd[received]=v[i]; else close(v[i]);
        }
    }
    if (n==sizeof(*p) && p->magic==FRAME_MAGIC && received==count &&
        !(msg.msg_flags&(MSG_CTRUNC|MSG_TRUNC))) return 1;
    for (int i=0;i<received && i<count;i++) close(fd[i]);
    return 0;
}
/* Diagnostic only. Pixel transport uses the dequeued allocation itself. */
static inline uint32_t frame_sample(const uint32_t *p,unsigned w,unsigned h,unsigned stride)
{
    uint32_t value=2166136261u;
    for(unsigned y=0;y<h;y+=h/13+1)
        for(unsigned x=0;x<w;x+=w/19+1)
            value=(value^p[(size_t)y*stride+x])*16777619u;
    return value;
}
