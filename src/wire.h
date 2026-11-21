#pragma once
#include "frame.h"
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/mman.h>
#define LAYER_INPUT_PATH "/data/local/tmp/waydroid-acc/input"
enum { SETTINGS, SYSTEMUI, LAUNCHER, ORDINARY, PRODUCERS };
static const char *const system_names[ORDINARY]={
    "com.android.settings","com.android.systemui","com.android.launcher3"
};
static inline int input_shape(unsigned w,unsigned h)
{
    return w>=16 && h>=16 && w<=4096 && h<=4096 && (uint64_t)w*h<=2359296;
}
static inline int input_layout(unsigned w,unsigned h,unsigned stride)
{
    return input_shape(w,h) && stride>=w && stride<=8192 && (uint64_t)stride*h<=2490368;
}
static inline int accepts_shape(int role,unsigned w,unsigned h)
{
    if(role==SETTINGS) return frame_full_height(h) && (w==698 || w==1222);
    if(role==SYSTEMUI || role==LAUNCHER) return w==1920 && (h==32 || h==132 || frame_full_height(h));
    return role==ORDINARY && input_shape(w,h);
}
#define LAYER_INPUT_STAMP 0x42544957u
enum {LAYER_INPUT_PUBLISH=29,LAYER_INPUT_GET,LAYER_INPUT_CPU,LAYER_INPUT_OK,LAYER_INPUT_MISS,LAYER_INPUT_REJECT,LAYER_INPUT_BOUND,APP_WINDOW_WINDOW,SYSTEM_WINDOW_SYSTEM_WINDOW,EMPTY_FRAME_EMPTY};
/* Publish reply sample bits. Older producers treat any nonzero value as confirmed. */
enum { LAYER_INPUT_SAMPLED=1, APP_WINDOW_CAN_IMPORT=2, SYSTEM_WINDOW_CAN_CHECK=4 };
struct input_stamp {uint64_t nonce; uint32_t serial, magic;};
static int layer_input_connect(void)
{
    int fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);
    if(fd<0) return -1;
    struct sockaddr_un addr={.sun_family=AF_UNIX,.sun_path=LAYER_INPUT_PATH};
    struct timeval limit={.tv_sec=2};
    struct ucred peer;
    socklen_t n=sizeof(peer);
    if(setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&limit,sizeof(limit)) ||
       setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&limit,sizeof(limit)) ||
       connect(fd,(void*)&addr,sizeof(addr)) ||
       getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&n) || peer.uid!=1000) {
        close(fd); return -1;
    }
    return fd;
}
static inline int layer_input_request(int phase,uint64_t id,int plane,struct input_stamp stamp,
                      struct frame_frame *reply,int *texture)
{
    int sd=layer_input_connect();
    if(sd<0) return 0;
    struct frame_frame p={.magic=FRAME_MAGIC,.phase=phase,.buffer_id=id,.serial=stamp.serial,
                       .sample=(uint32_t)stamp.nonce,.alpha_fix=stamp.nonce>>32};
    int rc=0,fd=-1;
    if(frame_packet(sd,&p,&plane,1,1) && recv(sd,&p,sizeof(p),MSG_PEEK)==sizeof(p)) {
        int nf=phase==LAYER_INPUT_GET && p.phase==LAYER_INPUT_OK;
        if(frame_packet(sd,&p,&fd,nf,0)) {
          if(p.buffer_id==id && p.serial==stamp.serial &&
           (p.phase==LAYER_INPUT_MISS || p.phase==LAYER_INPUT_OK)) {
            *reply=p;
            if(texture) *texture=fd;
            rc=p.phase;
          } else if(fd>=0) close(fd);
        }
    }
    close(sd);
    return rc;
}
