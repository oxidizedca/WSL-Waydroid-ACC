#include <openssl/sha.h>
#include <sys/random.h>
#include "ticket.h"

struct published {
    uint64_t id, nonce;
    uint32_t serial, stride, height, opaque;
    int texture;
    unsigned char digest[SHA256_DIGEST_LENGTH];
};
static struct published frames[8];
static unsigned next_frame;

static void weston_texture_publish(const struct frame_frame *p, void *map, int texture, int opaque)
{
    struct weston_texture_ticket mark={.serial=p->serial,.magic=WESTON_TEXTURE_MAGIC};
    if (getrandom(&mark.nonce,sizeof(mark.nonce),0)!=sizeof(mark.nonce) || !mark.nonce) return;
    int fd=fcntl(texture,F_DUPFD_CLOEXEC,3);
    if(fd<0) return;
    struct published *e=&frames[next_frame++ % 8];
    if(e->nonce) close(e->texture);
    *e=(struct published){.id=p->buffer_id,.nonce=mark.nonce,.serial=p->serial,
                          .stride=p->stride,.height=p->height,.opaque=opaque,.texture=fd};
    memcpy((uint8_t *)map+p->width*4,&mark,sizeof(mark));
    SHA256(map,(size_t)p->stride*p->height*4,e->digest);
}

/* The producer keeps a real Android slot for recording, CLIENT composition,
 * and fallback. A legacy writer may preserve padding, so also authenticate
 * its contents before lending a texture. The caller holds the mapper lock. */
static void weston_texture_fetch(int sock)
{
    struct frame_frame p;
    int fd=-1;
    if(!frame_packet(sock,&p,&fd,1,0)) return;
    uint64_t nonce=((uint64_t)p.alpha_fix<<32)|p.sample;
    for(unsigned i=0;i<8;i++) {
        struct published *e=&frames[i];
        if(!nonce || e->nonce!=nonce || e->id!=p.buffer_id || e->serial!=p.serial) continue;
        size_t size=(size_t)e->stride*e->height*4;
        struct stat st;
        if(fstat(fd,&st) || st.st_size<(off_t)size || sync_plane(fd,DMA_BUF_SYNC_START|DMA_BUF_SYNC_READ)) break;
        void *map=mmap(NULL,size,PROT_READ,MAP_SHARED,fd,0);
        int ok=0;
        if(map!=MAP_FAILED) {
            unsigned char digest[SHA256_DIGEST_LENGTH];
            SHA256(map,size,digest);
            ok=!memcmp(digest,e->digest,sizeof(digest));
            munmap(map,size);
        }
        if(sync_plane(fd,DMA_BUF_SYNC_END|DMA_BUF_SYNC_READ)) ok=0;
        if(ok) {
            p.phase=FRAME_DONE;
            p.nonempty=e->opaque;
            frame_packet(sock,&p,&e->texture,1,1);
        }
        break;
    }
    close(fd);
}
