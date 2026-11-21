#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <sys/system_properties.h>
#include <fcntl.h>
#include <sys/time.h>
#include "frame.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <stddef.h>
#include <poll.h>
#define PHYSICAL_GET 18
#define PHYSICAL_CPU 19
#define PHYSICAL_GPU 20
#define PHYSICAL_MIRROR 21
/* Android's abstract namespace follows the container lifetime. No stale
 * socket file and no dependency on a WSL helper to recover a physical slot. */
static socklen_t physical_address(struct sockaddr_un *a)
{
    const char name[]="waydroid.sf.physical.v1";
    memset(a,0,sizeof(*a));
    a->sun_family=AF_UNIX;
    memcpy(a->sun_path+1,name,sizeof(name)-1);
    return offsetof(struct sockaddr_un,sun_path)+sizeof(name);
}
static int physical_peer(int fd,const char *expected)
{
    struct ucred cred;
    socklen_t len=sizeof(cred);
    if(getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&cred,&len) || cred.uid!=1000) return 0;
    char pid[PROP_VALUE_MAX]={0};
    return __system_property_get(expected,pid)>0 && cred.pid>0 && atoi(pid)==cred.pid;
}
static void physical_timeout(int fd)
{
    struct timeval t={.tv_sec=1};
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&t,sizeof(t));
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&t,sizeof(t));
}
