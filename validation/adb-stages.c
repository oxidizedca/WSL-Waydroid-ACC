// Diagnostic only. Run as adb shell with cwd in a private (0700) Android tmpfs.
// stdout is exclusively the selected payload. No trace overhead on installed binaries.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const char *capture="/system/bin/screencap", *encode="/system_ext/bin/waydroid-acc-png";
static void ck(int x){if(x<0){perror("adb-stages");exit(1);}}
static int64_t now(void){struct timespec t;ck(clock_gettime(CLOCK_MONOTONIC,&t));return (int64_t)t.tv_sec*1000000000+t.tv_nsec;}
static void stamps(const char *file,int64_t a,int64_t b,int64_t c) {
    int f=open(file,O_CREAT|O_TRUNC|O_WRONLY|O_NOFOLLOW|O_CLOEXEC,0600);ck(f);
    ck(dprintf(f,"%" PRId64 " %" PRId64 " %" PRId64 "\n",a,b,c));ck(close(f));
}
static void child(const char *path,int in,int out) {
    pid_t p=fork();ck(p); if(!p){if(in>=0 && dup2(in,0)<0)_exit(125);if(dup2(out,1)<0)_exit(125);execl(path,path,(char*)NULL);_exit(127);}
    int s;while(waitpid(p,&s,0)<0)if(errno!=EINTR){perror("waitpid");exit(1);}
    if(!WIFEXITED(s)||WEXITSTATUS(s)!=0){fprintf(stderr,"child %s status %d\n",path,s);exit(1);}
}
static void send_png(int fd,size_t limit){
    struct stat s;ck(fstat(fd,&s));if(s.st_size<32 || s.st_size>16*1024*1024)exit(1);
    size_t n=s.st_size;
    unsigned char *p=mmap(NULL,n,PROT_READ,MAP_PRIVATE,fd,0);if(p==MAP_FAILED){perror("mmap");exit(1);}
    if(limit && limit<n)n=limit;
    while(n){ssize_t k=write(1,p,n>262144?262144:n);if(k<0 && errno==EINTR)continue; if(k<=0)exit(1); n-=k;p+=k;}
}
int main(int argc,char **argv){
    if(argc!=2)return 2;
    if(!strcmp(argv[1],"empty"))return 0;
    if(!strcmp(argv[1],"capture")){stamps("begin",now(),0,0);execl(capture,capture,(char*)NULL);return 1;}
    if(!strcmp(argv[1],"split")){
        int r=memfd_create("diag-raw",MFD_CLOEXEC);ck(r);
        int f=open("last.png",O_RDWR|O_CREAT|O_TRUNC|O_NOFOLLOW|O_CLOEXEC,0600);ck(f);
        int64_t a=now(); child(capture,-1,r);int64_t b=now();
        if(lseek(r,0,SEEK_SET)<0)return 1;
        child(encode,r,f);int64_t c=now();send_png(f,0);stamps("split-times",a,b,c);return 0;
    }
    size_t n=0;if(!strcmp(argv[1],"quarter"))n=262144;
    else if(!strcmp(argv[1],"one"))n=1048576;else if(strcmp(argv[1],"replay"))return 2;
    int fd=open("last.png",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);ck(fd);send_png(fd,n);return 0;
}
