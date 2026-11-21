#pragma once
#include "wire.h"
#define INPUT_SLOTS 64
/* One writer publishes an entire bank with one atomic epoch change. If the
 * broker dies mid-update the last complete bank remains available to Android. */
struct generation {uint64_t id,dev,ino,nonce,serial,magic;};
struct state_slot {uint64_t epoch; struct generation banks[2];};
static struct state_slot *state_table;
#define STATE_BYTES (sizeof(*state_table)*INPUT_SLOTS)
static inline int open_table(const char *path,int writer)
{
    int flags=writer ? O_RDWR|O_CREAT : O_RDONLY;
    int fd=open(path,flags|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0) return 0;
    struct stat st;
    int ok=!fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_uid==1000 &&
        (st.st_size==(off_t)STATE_BYTES || (writer && !st.st_size && !ftruncate(fd,STATE_BYTES)));
    void *map=ok ? mmap(NULL,STATE_BYTES,PROT_READ|(writer?PROT_WRITE:0),MAP_SHARED,fd,0) : MAP_FAILED;
    close(fd);
    if(map==MAP_FAILED) return 0;
    struct state_slot *empty=NULL;
    if(!__atomic_compare_exchange_n(&state_table,&empty,map,0,__ATOMIC_RELEASE,__ATOMIC_ACQUIRE))
        munmap(map,STATE_BYTES);
    return 1;
}
#define STATE_FIELDS(OP) OP(id) OP(dev) OP(ino) OP(nonce) OP(serial) OP(magic)
static inline void set_generation(unsigned index,struct generation g)
{
    struct state_slot *s=&state_table[index];
    uint64_t next=__atomic_load_n(&s->epoch,__ATOMIC_SEQ_CST)+1;
    struct generation *bank=&s->banks[next&1];
#define WRITE(f) __atomic_store_n(&bank->f,g.f,__ATOMIC_SEQ_CST);
    STATE_FIELDS(WRITE)
#undef WRITE
    __atomic_store_n(&s->epoch,next,__ATOMIC_SEQ_CST);
}
static inline struct input_stamp lookup_stamp(uint64_t id,const struct stat *st)
{
    struct input_stamp none={0};
    if(!__atomic_load_n(&state_table,__ATOMIC_ACQUIRE) && !open_table(LAYER_INPUT_PATH ".state",0))
        return none;
    for(unsigned i=0;i<INPUT_SLOTS;i++) {
        struct state_slot *s=&state_table[i];
        uint64_t epoch;
        struct generation g;
        do {
            epoch=__atomic_load_n(&s->epoch,__ATOMIC_SEQ_CST);
            struct generation *bank=&s->banks[epoch&1];
#define READ(f) g.f=__atomic_load_n(&bank->f,__ATOMIC_SEQ_CST);
            STATE_FIELDS(READ)
#undef READ
        } while(epoch!=__atomic_load_n(&s->epoch,__ATOMIC_SEQ_CST));
        if(g.id==id && g.dev==(uint64_t)st->st_dev && g.ino==(uint64_t)st->st_ino)
            return (struct input_stamp){g.nonce,g.serial,g.magic};
    }
    return none;
}
