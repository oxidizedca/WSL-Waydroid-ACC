#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <elf.h>
#include <dlfcn.h>
#include <signal.h>
#include <math.h>
#include <stdarg.h>
#include <sys/syscall.h>
#include <setjmp.h>
#include <limits.h>
#include <time.h>
#include <pthread.h>
#include <wchar.h>
#include <locale.h>
#include <ucontext.h>

typedef struct { uint32_t a; uint16_t b, c; uint8_t d[8]; } GUID;
static const GUID IID_ID3D12Device = {0x189819f1, 0x1db6, 0x4b57,
                                      {0xbe,0x54,0x18,0x21,0x33,0x9d,0x85,0xf7}};
typedef long (*d3d12_create_t)(void*, int, const GUID*, void**);

#define MAXLIBS 16
typedef struct {
    void *base;
    uint64_t minv, mapsize;
    Elf64_Sym *syms;
    const char *strs;
    size_t strsz, nsyms;
    uint64_t init_arr, init_arrsz, init_fn;
    void *eh_hdr;                 /* PT_GNU_EH_FRAME */
    Elf64_Phdr *phdrs;            /* 原始 program headers 的副本，供 dl_iterate_phdr 注入 */
    int phnum;
    void *tls_block;              /* 已废弃：每线程块改由 tls_state 管理 */
    size_t tls_size;
    const void *tls_init;         /* PT_TLS 初值镜像（p_filesz 那段） */
    size_t tls_init_size;
    char name[128];
} lib_t;
static lib_t g_libs[MAXLIBS];
static int g_nlibs;

/* ---------- shims ---------- */
static void *shim_memcpy(void *d, const void *s, size_t n) {
    volatile unsigned char *dd = d; const volatile unsigned char *ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}
static void *shim_memset(void *d, int c, size_t n) {
    volatile unsigned char *dd = d; unsigned char v = (unsigned char)c;
    while (n--) *dd++ = v;
    return d;
}
static void *shim_memmove(void *d, const void *s, size_t n) {
    volatile unsigned char *dd = d; const volatile unsigned char *ss = s;
    if (dd < ss) { while (n--) *dd++ = *ss++; }
    else { dd += n; ss += n; while (n--) *--dd = *--ss; }
    return d;
}
static int shim_cxa_atexit(void (*f)(void*), void *a, void *d) { (void)f;(void)a;(void)d; return 0; }
/* bionic 的 __cxa_thread_atexit_impl 会拿 dso_handle 去查自己的 soinfo，
 * 我们的模块它不认识 → 直接 abort。这里必须短路掉。 */
static int shim_cxa_thread_atexit_impl(void (*f)(void*), void *a, void *d) { (void)f;(void)a;(void)d; return 0; }
static void shim_cxa_finalize(void *d) { (void)d; }
/* core 的 "D3D12: Removing Device." 走 fwrite(ptr,0x18,1,stderr)，
 * 截住它就能拿到"是谁调的 removal helper"——也就是校验失败的调用点。 */
static unsigned long g_hook_rbp __attribute__((used)), g_hook_rsp __attribute__((used));
static const char *libshort(const char *n) {
    const char *s = strrchr(n, '/');
    return s ? s + 1 : n;
}
/* 找到 addr 落在哪个模块，返回模块下标，并给出 lib 内偏移 */
static int lib_of(unsigned long a, unsigned long *off) {
    for (int i = 0; i < g_nlibs; i++) {
        unsigned long bb = (unsigned long)g_libs[i].base;
        if (a >= bb && a < bb + g_libs[i].mapsize) { if (off) *off = a - bb; return i; }
    }
    return -1;
}
/* 判定 (模块下标 wi, 库内偏移 off) 是否落在该模块的 PF_X 段里 */
static int addr_is_exec(int wi, unsigned long off) {
    lib_t *l = &g_libs[wi];
    if (!l->phdrs) return 0;
    unsigned long va = off + (unsigned long)l->minv;
    for (int i = 0; i < l->phnum; i++) {
        if (l->phdrs[i].p_type != PT_LOAD) continue;
        if (!(l->phdrs[i].p_flags & PF_X)) continue;
        if (va >= l->phdrs[i].p_vaddr && va < l->phdrs[i].p_vaddr + l->phdrs[i].p_memsz) return 1;
    }
    return 0;
}
static size_t shim_fwrite_body(const void *p, size_t sz, size_t n, void *f) __attribute__((used));
/* naked 入口：removal helper 在 call fwrite 之前已经把 HRESULT 放进 %rbp
 * （0x46a6c3: mov %esi,%ebp），所以这里把 %rbp/%rsp 抓下来再转正文。 */
__attribute__((naked, used)) size_t shim_fwrite(const void *p, size_t sz, size_t n, void *f) {
    __asm__ __volatile__("movq %rbp, g_hook_rbp(%rip)\n\t"
                         "movq %rsp, g_hook_rsp(%rip)\n\t"
                         "jmp shim_fwrite_body\n\t");
}
static size_t shim_fwrite_body(const void *p, size_t sz, size_t n, void *f) {
    if (p && sz == 0x18 && !memcmp(p, "D3D12: Removing Device.", 18)) {
        unsigned long off;
        int i = lib_of((unsigned long)__builtin_return_address(0), &off);
        fprintf(stderr, "[hook] RemovingDevice hr=0x%08lx tid=%ld from %s+0x%lx\n",
                g_hook_rbp, (long)syscall(__NR_gettid),
                i >= 0 ? libshort(g_libs[i].name) : "?", off);
        /* 栈回溯：找 return address 落在移除 helper 之后的那个调用点。
         * helper=core+0x46a6c0(变体A)/+0x46a640(变体B)，call rel32 可直接验算。 */
        unsigned long core = 0;
        for (int k = 0; k < g_nlibs; k++)
            if (strstr(g_libs[k].name, "d3d12core")) core = (unsigned long)g_libs[k].base;
        unsigned long *sp = (unsigned long *)g_hook_rsp;
        for (int k = 0; k < 512; k++) {
            unsigned long w = sp[k];
            if (!w) continue;
            int wi = lib_of(w, &off);
            if (wi < 0) continue;
            /* 只认可执行段里的地址，避免把 rodata / vtable 数据当成返回地址 */
            if (!addr_is_exec(wi, off)) continue;
            const char *note = "";
            if (core && w > core && w < core + g_libs[wi].mapsize) {
                unsigned char *q = (unsigned char *)(w - 5);
                if (*q == 0xE8) {
                    int rel = *(int *)(w - 4);
                    unsigned long tgt = w + rel;
                    if (tgt == core + 0x46a640 || tgt == core + 0x46a6c0) note = "  <== removal helper caller";
                    else {
                        unsigned long toff;
                        int ti = lib_of(tgt, &toff);
                        if (ti >= 0) {
                            static char buf[96];
                            snprintf(buf, sizeof buf, "  (call %s+0x%lx)", libshort(g_libs[ti].name), toff);
                            fprintf(stderr, "  [stk+0x%02x] %s+0x%lx%s\n", k * 8, libshort(g_libs[wi].name), off, buf);
                            continue;
                        }
                    }
                }
            }
            fprintf(stderr, "  [stk+0x%02x] %s+0x%lx%s\n", k * 8,
                    libshort(g_libs[wi].name), off, note);
        }
    }
    return fwrite(p, sz, n, (FILE *)f);
}

/* 调试钩子：core 里的致命消息都走 fprintf(stderr,...)，截住它就能拿到
 * 出事调用方在库里的偏移（disassemble 过去就知道是哪条校验/哪个 API）。 */
static int shim_fprintf(void *f, const char *fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (fmt && (strstr(buf, "Removing Device") || strstr(buf, "Device removed"))) {
        unsigned long ra = (unsigned long)__builtin_return_address(0);
        int hit = -1;
        for (int i = 0; i < g_nlibs; i++) {
            unsigned long bb = (unsigned long)g_libs[i].base;
            if (ra >= bb && ra < bb + g_libs[i].mapsize) { hit = i; break; }
        }
        if (hit >= 0)
            fprintf(stderr, "[hook] \"%s\" from %s+0x%lx\n", buf,
                    strrchr(g_libs[hit].name, '/') ? strrchr(g_libs[hit].name, '/') + 1 : g_libs[hit].name,
                    ra - (unsigned long)g_libs[hit].base);
        else
            fprintf(stderr, "[hook] \"%s\" from 0x%lx (unknown)\n", buf, ra);
    }
    int r = fputs(buf, (FILE *)f);
    return r;
}
static int g_errno_stub;
static int *shim_errno(void) { return &g_errno_stub; }
static int shim_bcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
static void shim_assert_fail(const char *a, const char *b, unsigned int c, const char *d) {
    fprintf(stderr, "[assert] %s %s (%u) at %s\n", a?a:"", b?b:"", c, d?d:"");
    abort();
}
struct tls_index { size_t module; size_t offset; };
/* module id = (lib - g_libs) + 1；真正的 TLS 块按线程分配，见 lib_tls_block()。
 * 拿不到线程状态时退化成这块共享内存：语义不对但至少不崩。 */
static char g_tls_sink[0x1000];
static void *tls_block_by_mod(int mod);
static int shim_ioctl_probe(int fd, unsigned long req, ...);
static long shim_sysconf_compat(int name);
static void r69_tls_sink_note(int mod, size_t off, size_t sz);
static void *shim_tls_get_addr(struct tls_index *ti) {
    if (!ti || !ti->module) return g_tls_sink;
    int mod = (int)ti->module;
    void *blk = tls_block_by_mod(mod);
    if (!blk) { r69_tls_sink_note(mod, ti->offset, 0); return g_tls_sink + (ti->offset & 0xfff); }
    if (mod <= MAXLIBS && g_libs[mod - 1].tls_size && ti->offset >= g_libs[mod - 1].tls_size) {
        r69_tls_sink_note(mod, ti->offset, g_libs[mod - 1].tls_size);
        return g_tls_sink + (ti->offset & 0xfff);
    }
    return (char *)blk + ti->offset;
}
static void *shim_dlopen(const char *p, int f);
static void *shim_dlsym(void *h, const char *n);
static int shim_dlclose(void *h) { (void)h; return 0; }

/* ---------- glibc 布局的 pthread mutex（bionic 不认 __kind，递归锁会死锁） ----------
 * glibc pthread_mutex_t: __lock(0) __count(4) __owner(8) __nusers(12) __kind(16) ...
 * __kind: 0=PTHREAD_MUTEX_TIMED_NP, 1=PTHREAD_MUTEX_RECURSIVE_NP
 */
#define FUTEX_WAIT_PRIVATE 128
#define FUTEX_WAKE_PRIVATE 129
/* 注意：**不能缓存**。以前这里缓存成进程级全局，等于把"第一个线程的 tid"
 * 当成所有线程的 tid —— 按 tid 建 TLS 表时全进程会共用一份镜像，
 * 线程之间互相把对方的 dispatch 覆盖掉（HWUI 的 RenderThread/hwuiTask 就是多线程）。 */
static int my_tid(void) { return (int)syscall(__NR_gettid); }
static int futex_op(int *uaddr, int op, int val) {
    return (int)syscall(__NR_futex, uaddr, op, val, NULL, NULL, 0);
}
static int shim_mutex_init(void *m, const void *attr) {
    memset(m, 0, 40);
    int kind = 0;
    if (attr) kind = (*(const int *)attr) & 3;
    *(int *)((char *)m + 16) = kind;
    return 0;
}
static int shim_mutex_lock(void *m) {
    volatile int *lock = (int *)m;
    unsigned *count = (unsigned *)((char *)m + 4);
    int *owner = (int *)((char *)m + 8);
    int *kind = (int *)((char *)m + 16);
    for (;;) {
        int expect = 0;
        if (__atomic_compare_exchange_n(lock, &expect, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            *owner = my_tid();
            return 0;
        }
        if (*kind == 1 && *owner == my_tid()) { *count += 1; return 0; }
        __atomic_store_n(lock, 2, __ATOMIC_ACQUIRE);
        futex_op((int *)lock, FUTEX_WAIT_PRIVATE, 2);
    }
}
static int shim_mutex_trylock(void *m) {
    volatile int *lock = (int *)m;
    int *owner = (int *)((char *)m + 8);
    int *kind = (int *)((char *)m + 16);
    unsigned *count = (unsigned *)((char *)m + 4);
    if (__atomic_load_n(lock, __ATOMIC_ACQUIRE) == 0) {
        int expect = 0;
        if (__atomic_compare_exchange_n(lock, &expect, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            *owner = my_tid();
            return 0;
        }
    }
    if (*kind == 1 && *owner == my_tid()) { *count += 1; return 0; }
    return 16;   /* EBUSY */
}
static int shim_mutex_unlock(void *m) {
    volatile int *lock = (int *)m;
    int *owner = (int *)((char *)m + 8);
    unsigned *count = (unsigned *)((char *)m + 4);
    int *kind = (int *)((char *)m + 16);
    if (*kind == 1 && *count > 0) { *count -= 1; return 0; }
    *owner = 0;
    int old = __atomic_exchange_n(lock, 0, __ATOMIC_RELEASE);
    if (old == 2) futex_op((int *)lock, FUTEX_WAKE_PRIVATE, 1);
    return 0;
}
static int shim_mutex_destroy(void *m) { (void)m; return 0; }
static int shim_mutexattr_init(void *a) { *(int *)a = 0; return 0; }
static int shim_mutexattr_destroy(void *a) { (void)a; return 0; }
static int shim_mutexattr_settype(void *a, int t) { *(int *)a = (*(int *)a & ~3) | (t & 3); return 0; }
static int shim_mutexattr_gettype(const void *a, int *t) { *t = (*(const int *)a) & 3; return 0; }

/* ---------- glibc 布局的条件变量（bionic 的 condattr 是 8 字节，会踩坏 glibc 的 4 字节栈槽） ----------
 * glibc pthread_cond_t: 48 字节；pthread_condattr_t: 4 字节。
 * 这里用 "序号 + futex" 自己实现，与上面的 mutex 配套。
 */
static int shim_condattr_init(void *a) { *(int *)a = 0; return 0; }
static int shim_condattr_destroy(void *a) { (void)a; return 0; }
static int shim_condattr_setclock(void *a, int clock) { *(int *)a = clock; return 0; }
static int shim_condattr_getclock(const void *a, int *clock) { *clock = *(const int *)a; return 0; }
static int shim_cond_init(void *c, const void *attr) {
    memset(c, 0, 48);
    int clock = attr ? *(const int *)attr : 0;   /* 0 = CLOCK_REALTIME */
    *(int *)((char *)c + 4) = clock;
    return 0;
}
static int shim_cond_destroy(void *c) { (void)c; return 0; }
static int shim_cond_signal(void *c) {
    __atomic_add_fetch((int *)c, 1, __ATOMIC_RELEASE);
    futex_op((int *)c, FUTEX_WAKE_PRIVATE, 1);
    return 0;
}
static int shim_cond_broadcast(void *c) {
    __atomic_add_fetch((int *)c, 1, __ATOMIC_RELEASE);
    futex_op((int *)c, FUTEX_WAKE_PRIVATE, 0x7fffffff);
    return 0;
}
static int shim_cond_wait(void *c, void *m) {
    int seq = __atomic_load_n((int *)c, __ATOMIC_ACQUIRE);
    shim_mutex_unlock(m);
    futex_op((int *)c, FUTEX_WAIT_PRIVATE, seq);
    shim_mutex_lock(m);
    return 0;
}
static int shim_cond_timedwait(void *c, void *m, const struct timespec *abstime) {
    int clock = *(int *)((char *)c + 4);
    int seq = __atomic_load_n((int *)c, __ATOMIC_ACQUIRE);
    struct timespec now;
    if (clock == 1) clock_gettime(CLOCK_MONOTONIC, &now);
    else clock_gettime(CLOCK_REALTIME, &now);
    long long ms = (long long)(abstime->tv_sec - now.tv_sec) * 1000
                 + (abstime->tv_nsec - now.tv_nsec) / 1000000;
    shim_mutex_unlock(m);
    if (ms > 0) {
        struct timespec rel;
        rel.tv_sec = ms / 1000;
        rel.tv_nsec = (ms % 1000) * 1000000;
        syscall(__NR_futex, (int *)c, FUTEX_WAIT_PRIVATE, seq, &rel, NULL, 0);
    }
    shim_mutex_lock(m);
    return 0;
}

/* ---------- pthread_once：不能用 bionic 的实现 ----------
 * bionic 的 pthread_once 内部是一把【全局非递归 mutex】+ 对象里的 ONCE_RUNNING 状态。
 * glibc 侧代码（Mesa 的 util_call_once 之类）会在同一线程里【嵌套】调用两次
 * pthread_once（不同的 once 对象），glibc 允许（它按对象加锁，没有全局锁），
 * bionic 则是同一个线程再去抢自己持有的全局锁 → 自锁，进程永远停在
 * __futex_wait_ex(1) 上。现象就是 eglCreateContext 挂死、5 个线程里主线程卡 pthread_once。
 * 这里按 glibc 语义重写：状态放对象里，全局锁做成同线程可重入。 */
static int env_on(const char *n);   /* 定义在文件后面 */
static void dbg_s(const char *s);
static void dbg_hex(unsigned long v);
static void dbg_ptr(const void *p);
static void dbg_dec(long v);
static int tls_dbg_on(void);
static struct { int lock, depth, owner; } g_once_lock;
static int once_lock_acquire(void) {
    int me = my_tid();
    for (;;) {
        int expect = 0;
        if (__atomic_compare_exchange_n(&g_once_lock.lock, &expect, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            g_once_lock.owner = me; g_once_lock.depth = 1; return 0;
        }
        if (g_once_lock.owner == me && g_once_lock.depth > 0) { g_once_lock.depth++; return 0; }
        __atomic_store_n(&g_once_lock.lock, 2, __ATOMIC_ACQUIRE);
        futex_op(&g_once_lock.lock, FUTEX_WAIT_PRIVATE, 2);
    }
}
static void once_lock_release(void) {
    if (g_once_lock.owner != my_tid()) return;
    if (--g_once_lock.depth > 0) return;
    g_once_lock.owner = 0;
    int old = __atomic_exchange_n(&g_once_lock.lock, 0, __ATOMIC_RELEASE);
    if (old == 2) futex_op(&g_once_lock.lock, FUTEX_WAKE_PRIVATE, 1);
}
static int g_once_dbg;
static int shim_pthread_once(void *once, void (*init)(void)) {
    volatile int *st = (int *)once;
    if (!once || !init) return 22;                       /* EINVAL */
    if (__atomic_load_n(st, __ATOMIC_ACQUIRE) == 2) return 0;
    once_lock_acquire();
    int nested_self = 0;
    if (__atomic_load_n(st, __ATOMIC_ACQUIRE) != 2) {
        if (*st == 1) {
            /* 同一个 once 对象自我递归：glibc 在这里会死等，我们直接放行 */
            nested_self = 1;
        } else {
            if (env_on("ONCE_DBG") && g_once_dbg < 30) {
                g_once_dbg++;
                dbg_s("[once] tid="); dbg_dec(my_tid()); dbg_s(" once="); dbg_ptr(once);
                dbg_s(" depth="); dbg_dec(g_once_lock.depth); dbg_s("\n");
            }
            *st = 1;
            init();
            __atomic_store_n(st, 2, __ATOMIC_RELEASE);
        }
    }
    once_lock_release();
    if (nested_self && env_on("ONCE_DBG") && g_once_dbg < 30) {
        g_once_dbg++;
        dbg_s("[once] tid="); dbg_dec(my_tid()); dbg_s(" once="); dbg_ptr(once); dbg_s(" SELF-RECURSION skipped\n");
    }
    return 0;
}

/* glibc 的 call_once（<threads.h>，glibc 2.34+ 在 libc 里）与 pthread_once 一样会被
 * glibc 侧代码嵌套调用：Mesa 的 util_call_once_data_slow 就是走 call_once 的 PLT。
 * bionic 的 call_once 内部又转它自己的 pthread_once（同一把全局锁），所以嵌套照样自锁。
 * 这里让 call_once / __call_once 也走上面那套"按对象状态 + 同线程可重入全局锁"的实现。 */
static int shim_call_once(void *flag, void (*fn)(void)) {
    volatile int *st = (int *)flag;
    if (!flag || !fn) return 22;
    if (__atomic_load_n(st, __ATOMIC_ACQUIRE) == 2) return 0;
    once_lock_acquire();
    if (__atomic_load_n(st, __ATOMIC_ACQUIRE) != 2 && *st != 1) {
        if (env_on("ONCE_DBG") && g_once_dbg < 30) {
            g_once_dbg++;
            dbg_s("[once] call_once tid="); dbg_dec(my_tid()); dbg_s(" flag="); dbg_ptr(flag);
            dbg_s(" depth="); dbg_dec(g_once_lock.depth); dbg_s("\n");
        }
        *st = 1;
        fn();
        __atomic_store_n(st, 2, __ATOMIC_RELEASE);
    }
    once_lock_release();
    return 0;
}

/* ---------- ctype 表（glibc 的 __ctype_b_loc 等） ---------- */
static unsigned short g_ctype_b[384];
static int32_t g_ctype_lower[384], g_ctype_upper[384];
static int g_ctype_ready;
static void init_ctype(void) {
    if (g_ctype_ready) return;
    g_ctype_ready = 1;
    for (int i = 0; i < 384; i++) {
        int c = i - 128;
        unsigned short f = 0;
        if (c >= 0 && c < 256) {
            if (c >= 'A' && c <= 'Z') f |= 0x0100 | 0x0400 | 0x0008;      /* upper alpha alnum */
            if (c >= 'a' && c <= 'z') f |= 0x0200 | 0x0400 | 0x0008;      /* lower alpha alnum */
            if (c >= '0' && c <= '9') f |= 0x0800 | 0x0008;               /* digit alnum */
            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) f |= 0x1000;
            if (c == ' ' || c == '\t') f |= 0x2000 | 0x0001;
            if (c == '\n' || c == '\r' || c == '\v' || c == '\f') f |= 0x2000;
            if (c >= 0x20 && c < 0x7f) f |= 0x4000 | 0x8000;              /* print graph */
            if (c < 0x20 || c == 0x7f) f |= 0x0002;                       /* cntrl */
            if (c >= 0x21 && c < 0x7f && !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
                f |= 0x0004;                                              /* punct */
        }
        g_ctype_b[i] = f;
        g_ctype_lower[i] = (c >= 'A' && c <= 'Z') ? c + 32 : (c >= 0 && c < 256 ? c : 0);
        g_ctype_upper[i] = (c >= 'a' && c <= 'z') ? c - 32 : (c >= 0 && c < 256 ? c : 0);
    }
}
static unsigned short **shim_ctype_b_loc(void) { init_ctype(); return (unsigned short **)(g_ctype_b + 128); }
static int32_t **shim_ctype_tolower_loc(void) { init_ctype(); return (int32_t **)(g_ctype_lower + 128); }
static int32_t **shim_ctype_toupper_loc(void) { init_ctype(); return (int32_t **)(g_ctype_upper + 128); }
static char *shim_xpg_basename(char *p) {
    if (!p || !*p) return (char *)".";
    char *s = strrchr(p, '/');
    if (!s) return p;
    if (s[1]) return s + 1;
    return (char *)"/";
}
static int shim_printf_chk(int flag, const char *fmt, ...) {
    (void)flag;
    va_list ap; va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}
static int shim_vfprintf_chk(FILE *f, int flag, const char *fmt, va_list ap) {
    (void)flag;
    return vfprintf(f, fmt, ap);
}
static int shim_vswprintf_chk(wchar_t *s, size_t n, int flag, size_t slen, const wchar_t *fmt, va_list ap) {
    (void)flag; (void)slen;
    return vswprintf(s, n, fmt, ap);
}
static void shim_longjmp_chk(void *env, int val) { longjmp((struct __jmp_buf_tag *)env, val); }
static size_t shim_mbstowcs_chk(wchar_t *d, const char *s, size_t n, size_t dl) { (void)dl; return mbstowcs(d, s, n); }
static wchar_t *shim_wcsncpy_chk(wchar_t *d, const wchar_t *s, size_t n, size_t dl) { (void)dl; return wcsncpy(d, s, n); }
static char *shim_realpath_chk(const char *p, char *r, size_t rl) { (void)rl; return realpath(p, r); }
struct glibc_mallinfo2 { size_t arena, ordblks, smblks, hblks, hblkhd, usmblks, fsmblks, uordblks, fordblks, keepcost; };
static struct glibc_mallinfo2 shim_mallinfo2(void) {
    struct glibc_mallinfo2 mi;
    memset(&mi, 0, sizeof mi);
    return mi;
}

/* glibc 的 ISO C99 变体 + 程序名符号 */
static int shim_isoc99_sscanf(const char *s, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsscanf(s, fmt, ap);
    va_end(ap);
    return r;
}
static char g_prog_name_buf[] = "amdxc64.so";
static char *g_prog_short_name = g_prog_name_buf;
static char *g_prog_full_name = g_prog_name_buf;

/* --- libstdc++ / glibc 内部 locale & _chk 变体 --- */
static char g_single_threaded = 1;      /* glibc: __libc_single_threaded，数据符号 */
static int shim_fprintf_chk(FILE *f, int flag, const char *fmt, ...) {
    (void)flag;
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}
static const char *shim_gettext(const char *s) { return s ? s : ""; }
static const char *shim_dgettext(const char *d, const char *s) { (void)d; return s ? s : ""; }
static char *shim_bind_textdomain_codeset(const char *d, const char *c) { (void)d; (void)c; return NULL; }
static char *shim_bindtextdomain(const char *d, const char *c) { (void)d; return (char*)c; }
static void *shim_duplocale(void *l) { (void)l; return (void*)1; }
static char *shim_nl_langinfo_l(int item, void *l) {
    (void)l;
    static char *(*f)(int);
    if (!f) f = (char *(*)(int))dlsym(RTLD_DEFAULT, "nl_langinfo");
    return f ? f(item) : (char*)"";
}
static size_t shim_strftime_l(char *s, size_t n, const char *f, const struct tm *tm, void *l) {
    (void)l; return strftime(s, n, f, tm);
}
static size_t shim_strxfrm_l(char *d, const char *s, size_t n, void *l) { (void)l; return strxfrm(d, s, n); }
static float shim_strtof_l(const char *s, char **e, void *l) { (void)l; return strtof(s, e); }
static int shim_dl_find_object(void *pc, void *res) { (void)pc; (void)res; return -1; }
static void dump_stack(unsigned long rsp, int max);
static void *find_symbol_in(lib_t *l, const char *name);
static const char *mesa_dir(void);
static long shim_ret0(void) { return 0; }
static void shim_stack_chk_fail(void) {
    void *ra = __builtin_return_address(0);
    Dl_info di; memset(&di, 0, sizeof di);
    dladdr(ra, &di);
    fprintf(stderr, "\n[stack-chk] canary broken, detected in %s+0x%lx (ra=%p)\n",
            di.dli_fname ? di.dli_fname : "?", di.dli_fbase ? (unsigned long)ra - (unsigned long)di.dli_fbase : 0, ra);
    dump_stack((unsigned long)__builtin_frame_address(0), 20);
    abort();
}
long shim_weak_stub(void) {
    void *ra = __builtin_return_address(0);
    Dl_info di; memset(&di, 0, sizeof di);
    dladdr(ra, &di);
    fprintf(stderr, "  [weak-stub] called from %s+0x%lx\n",
            di.dli_fname ? di.dli_fname : "?", di.dli_fbase ? (unsigned long)ra - (unsigned long)di.dli_fbase : 0);
    return 0;
}
static char *shim_secure_getenv(const char *n) { return getenv(n); }
static void *shim_ret_null(void) { return NULL; }
/* glibc 的 LC_*_MASK 和 bionic 完全不同，转发给 bionic newlocale 会返回 NULL，
 * libstdc++ 就抛 std::runtime_error。这里统一返回自己的非空哨兵：libstdc++ 只把它
 * 当 token 存着，最后又交回本文件的 shim，不会真的进 bionic。 */
static char g_locale_sentinel;
static void *shim_locale_dl(const char *sym, void *a, void *b, void *c) {
    (void)sym; (void)a; (void)b; (void)c;
    return &g_locale_sentinel;
}
static void *shim_newlocale(void *a, void *b, void *c) { return shim_locale_dl("newlocale", a, b, c); }
static void *shim_uselocale(void *a, void *b, void *c) { (void)b;(void)c; return shim_locale_dl("uselocale", a, NULL, NULL); }
static double shim_strtod_l(const char *s, char **e, void *loc) { (void)loc; return strtod(s, e); }
static long shim_strtol_l(const char *s, char **e, int base, void *loc) { (void)loc; return strtol(s, e, base); }
static unsigned long shim_strtoul_l(const char *s, char **e, int base, void *loc) { (void)loc; return strtoul(s, e, base); }
static size_t shim_mbsrtowcs_chk(wchar_t *d, const char **s, size_t n, mbstate_t *st, size_t dl) { (void)dl; return mbsrtowcs(d, s, n, st); }
static wchar_t *shim_wmemcpy_chk(wchar_t *d, const wchar_t *s, size_t n, size_t dl) { (void)dl; return wmemcpy(d, s, n); }
static wchar_t *shim_wmemset_chk(wchar_t *d, wchar_t c, size_t n, size_t dl) { (void)dl; return wmemset(d, c, n); }
static int shim_wcscoll_l(const wchar_t *a, const wchar_t *b, void *l) { (void)l; return wcscmp(a, b); }
static int shim_strcoll_l(const char *a, const char *b, void *l) { (void)l; return strcmp(a, b); }

/* ---------- dl_iterate_phdr 注入 ----------
 * libdxcore / libd3d12core / libd3d12 全都【静态链接】了 LLVM libunwind
 * （strings 里能看到 /home/vsts/.../llvm-project/libunwind/src/UnwindLevel1.c），
 * 它靠 dl_iterate_phdr 遍历进程里的 DSO 来查 PT_GNU_EH_FRAME。
 * 我们的模块是匿名 mmap 出来的，bionic 的 linker 完全不知道它们 →
 * unwinder 找不到 FDE → 任何 C++ 异常（哪怕 throw 和 catch 在同一个库里）
 * 都变成 "terminating with uncaught exception" → abort。
 * 这里把 bionic 的遍历结果原样转发，再补上我们自己的模块条目。 */
struct dl_phdr_info {
    Elf64_Addr dlpi_addr;
    const char *dlpi_name;
    const Elf64_Phdr *dlpi_phdr;
    Elf64_Half dlpi_phnum;
    unsigned long long dlpi_adds;
    unsigned long long dlpi_subs;
};
typedef int (*dl_iterate_cb_t)(struct dl_phdr_info *, size_t, void *);
struct iter_ctx { dl_iterate_cb_t cb; void *data; };
static int iter_wrapper(struct dl_phdr_info *info, size_t sz, void *data) {
    struct iter_ctx *c = (struct iter_ctx *)data;
    return c->cb(info, sz, c->data);
}
static int shim_dl_iterate_phdr(dl_iterate_cb_t cb, void *data) {
    static int (*real)(dl_iterate_cb_t, void *);
    if (!real) real = (int (*)(dl_iterate_cb_t, void *))dlsym(RTLD_DEFAULT, "dl_iterate_phdr");
    struct iter_ctx c;
    c.cb = cb; c.data = data;
    if (real) {
        int r = real(iter_wrapper, &c);
        if (r) return r;
    }
    { const char *np = getenv("NO_PHDR_INJECT"); if (np && *np && *np != '0') return 0; }
    for (int i = 0; i < g_nlibs; i++) {
        lib_t *l = &g_libs[i];
        if (!l->eh_hdr || !l->phdrs || !l->phnum) continue;
        struct dl_phdr_info info;
        memset(&info, 0, sizeof info);
        info.dlpi_addr = (Elf64_Addr)((char *)l->base - l->minv);
        info.dlpi_name = l->name;
        info.dlpi_phdr = l->phdrs;
        info.dlpi_phnum = (Elf64_Half)l->phnum;
        int r = cb(&info, sizeof info, data);
        if (r) return r;
    }
    return 0;
}

/* --- Mesa 需要的额外 glibc 兼容 shim --- */
static int shim_vasprintf_chk(char **strp, int flag, const char *fmt, va_list ap) {
    (void)flag;
    va_list aq; va_copy(aq, ap);
    int n = vsnprintf(NULL, 0, fmt, aq);
    va_end(aq);
    if (n < 0) { *strp = NULL; return -1; }
    char *b = (char *)malloc((size_t)n + 1);
    if (!b) { *strp = NULL; return -1; }
    vsnprintf(b, (size_t)n + 1, fmt, ap);
    *strp = b;
    return n;
}
static int shim_asprintf_chk(char **strp, int flag, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = shim_vasprintf_chk(strp, flag, fmt, ap);
    va_end(ap);
    return r;
}
static void shim_syslog_chk(int pri, int flag, const char *fmt, ...) {
    (void)flag; (void)pri;
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
static int shim_fcntl64(int fd, int cmd, ...) {
    va_list ap; va_start(ap, cmd);
    long arg = va_arg(ap, long);
    va_end(ap);
    return fcntl(fd, cmd, arg);
}

static void shim_abort(void);

/* ---- r69 syscall 探针：UMD 的内存分配可能绕过 libc，直接 syscall(SYS_mmap) ----
 * 只记 mmap/munmap/mprotect/mremap 与任何失败调用，日志走裸 write 避免重入。 */
static void r69s_out(const char *s, unsigned n) { if (n) syscall(SYS_write, 2, s, n); }
static void r69s_hex(unsigned long v) {
    char b[20]; int i = 19; b[19] = 0;
    if (!v) { r69s_out("0", 1); return; }
    while (v && i > 0) { int d = (int)(v & 15); b[--i] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v >>= 4; }
    r69s_out(b + i, (unsigned)(19 - i));
}
static void r69s_dec(long v) {
    char b[24]; int i = 23; b[23] = 0;
    int neg = (v < 0); unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    if (!u) { r69s_out("0", 1); return; }
    while (u && i > 0) { b[--i] = (char)('0' + (u % 10)); u /= 10; }
    if (neg) b[--i] = '-';
    r69s_out(b + i, (unsigned)(23 - i));
}
static int (*g_logw)(int, const char *, const char *);
static int ap_str(char *b, int n, const char *s);
static int ap_hex(char *b, int n, unsigned long v);
static int r69_dec(char *b, int k, long v) {
    char t[24]; int n = 0;
    if (v < 0) { b[k++] = '-'; v = -v; }
    if (v == 0) t[n++] = '0';
    while (v > 0) { t[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while (n > 0) b[k++] = t[--n];
    return k;
}
static long shim_sysconf_compat(int name) {
    /* r69: translate glibc _SC_* constants to bionic _SC_* (values differ) */
    int bn = name;
    if (name == 30) bn = 39;        /* _SC_PAGESIZE */
    else if (name == 83) bn = 96;   /* _SC_NPROCESSORS_CONF */
    else if (name == 84) bn = 97;   /* _SC_NPROCESSORS_ONLN */
    else if (name == 85) bn = 98;   /* _SC_PHYS_PAGES */
    else if (name == 86) bn = 99;   /* _SC_AVPHYS_PAGES */
    long r = sysconf(bn);
    if (name == 30 && g_logw) {
        static int once30;
        if (!once30) {
            once30 = 1;
            char lb[64]; int k = 0;
            k = ap_str(lb, k, "SYSCONF compat pagesize="); k = r69_dec(lb, k, r);
            lb[k] = 0;
            g_logw(4, "d3d12ldr", lb);
        }
    }
    return r;
}
static long shim_syscall_probe(long n, ...) {
    va_list ap; va_start(ap, n);
    long a1 = va_arg(ap, long), a2 = va_arg(ap, long), a3 = va_arg(ap, long);
    long a4 = va_arg(ap, long), a5 = va_arg(ap, long), a6 = va_arg(ap, long);
    va_end(ap);
    long r = syscall(n, a1, a2, a3, a4, a5, a6);
    static int cnt;
    int interesting = (n == 9 || n == 10 || n == 11 || n == 25 || n == 216 || n == 28);
    int failed = (r < 0 && r > -4096);
    if ((interesting || failed) && cnt < 400) {
        cnt++;
        r69s_out("[r69sys] pid=", 12); r69s_dec((long)syscall(SYS_getpid));
        r69s_out(" n=", 3); r69s_dec(n);
        r69s_out(" a1=", 4); r69s_hex((unsigned long)a1);
        r69s_out(" a2=", 4); r69s_hex((unsigned long)a2);
        r69s_out(" a3=", 4); r69s_hex((unsigned long)a3);
        r69s_out(" r=", 3); r69s_hex((unsigned long)r);
        r69s_out("\n", 1);
    }
    return r;
}

static void *shim_lookup(const char *nm) {
    if (!nm) return NULL;
    if (!strcmp(nm,"syscall")) return (void*)shim_syscall_probe;
    if (!strcmp(nm,"sysconf")) return (void*)shim_sysconf_compat;
    if (!strcmp(nm,"memcpy")) return (void*)shim_memcpy;
    if (!strcmp(nm,"memset")) return (void*)shim_memset;
    if (!strcmp(nm,"memmove")) return (void*)shim_memmove;
    if (!strcmp(nm,"__memset_chk")) return (void*)shim_memset;
    if (!strcmp(nm,"__cxa_atexit")) return (void*)shim_cxa_atexit;
    if (!strcmp(nm,"__cxa_finalize")) return (void*)shim_cxa_finalize;
    if (!strcmp(nm,"__cxa_thread_atexit_impl")) return (void*)shim_cxa_thread_atexit_impl;
    if (!strcmp(nm,"__cxa_thread_atexit")) return (void*)shim_cxa_thread_atexit_impl;
    if (!strcmp(nm,"__errno_location")) return (void*)shim_errno;
    if (!strcmp(nm,"bcmp")) return (void*)shim_bcmp;
    if (!strcmp(nm,"__assert_fail")) return (void*)shim_assert_fail;
    if (!strcmp(nm,"__tls_get_addr")) return (void*)shim_tls_get_addr;
    if (!strcmp(nm,"ioctl")) return (void*)shim_ioctl_probe;
    if (!strcmp(nm,"ceil")) return (void*)ceil;
    if (!strcmp(nm,"ceilf")) return (void*)ceilf;
    if (!strcmp(nm,"log2")) return (void*)log2;
    if (!strcmp(nm,"dlopen")) return (void*)shim_dlopen;
    if (!strcmp(nm,"dlsym")) return (void*)shim_dlsym;
    if (!strcmp(nm,"dlclose")) return (void*)shim_dlclose;
    if (!strcmp(nm,"dl_iterate_phdr")) return (void*)shim_dl_iterate_phdr;
    if (!strcmp(nm,"pthread_once")) return (void*)shim_pthread_once;
    if (!strcmp(nm,"call_once")) return (void*)shim_call_once;
    if (!strcmp(nm,"__call_once")) return (void*)shim_call_once;
    if (!strcmp(nm,"abort")) return (void*)shim_abort;
    if (!strcmp(nm,"fprintf")) return (void*)shim_fprintf;
    if (!strcmp(nm,"fwrite")) return (void*)shim_fwrite;
    /* glibc 内部 / fortified / locale 变体 */
    if (!strcmp(nm,"secure_getenv")) return (void*)shim_secure_getenv;
    if (!strcmp(nm,"__newlocale")) return (void*)shim_newlocale;
    if (!strcmp(nm,"newlocale")) return (void*)shim_newlocale;
    if (!strcmp(nm,"__uselocale")) return (void*)shim_uselocale;
    if (!strcmp(nm,"__freelocale")) return (void*)shim_ret0;
    if (!strcmp(nm,"__strtod_l")) return (void*)shim_strtod_l;
    if (!strcmp(nm,"__strtol_l")) return (void*)shim_strtol_l;
    if (!strcmp(nm,"__strtoul_l")) return (void*)shim_strtoul_l;
    if (!strcmp(nm,"__isoc23_strtoul")) return (void*)shim_strtoul_l;
    if (!strcmp(nm,"__isoc23_strtol")) return (void*)strtol;
    if (!strcmp(nm,"__isoc23_strtoll")) return (void*)strtoll;
    if (!strcmp(nm,"__isoc23_strtoull")) return (void*)strtoull;
    if (!strcmp(nm,"__isoc23_fscanf")) return (void*)fscanf;
    if (!strcmp(nm,"fcntl64")) return (void*)shim_fcntl64;
    if (!strcmp(nm,"__asprintf_chk")) return (void*)shim_asprintf_chk;
    if (!strcmp(nm,"__vasprintf_chk")) return (void*)shim_vasprintf_chk;
    if (!strcmp(nm,"__syslog_chk")) return (void*)shim_syslog_chk;
    if (!strcmp(nm,"__getdelim")) return (void*)getdelim;
    if (!strcmp(nm,"__mbsrtowcs_chk")) return (void*)shim_mbsrtowcs_chk;
    if (!strcmp(nm,"__wmemcpy_chk")) return (void*)shim_wmemcpy_chk;
    if (!strcmp(nm,"__wmemset_chk")) return (void*)shim_wmemset_chk;
    if (!strcmp(nm,"__wcscoll_l")) return (void*)shim_wcscoll_l;
    if (!strcmp(nm,"__strcoll_l")) return (void*)shim_strcoll_l;
    if (!strcmp(nm,"__towupper_l")) return (void*)towupper;
    if (!strcmp(nm,"__towlower_l")) return (void*)towlower;
    if (!strcmp(nm,"__wctype_l")) return (void*)shim_ret_null;
    if (!strcmp(nm,"__iswctype_l")) return (void*)shim_ret0;
    if (!strcmp(nm,"__wcsxfrm_l")) return (void*)shim_ret0;
    if (!strcmp(nm,"__wcsftime_l")) return (void*)shim_ret0;
    if (!strcmp(nm,"strfromf128")) return (void*)shim_ret0;
    if (!strcmp(nm,"strtof128")) return (void*)shim_ret_null;
    if (!strcmp(nm,"bind_textdomain_codeset")) return (void*)shim_bind_textdomain_codeset;
    if (!strcmp(nm,"__libc_single_threaded")) return (void*)&g_single_threaded;
    if (!strcmp(nm,"__fprintf_chk")) return (void*)shim_fprintf_chk;
    if (!strcmp(nm,"gettext")) return (void*)shim_gettext;
    if (!strcmp(nm,"dgettext")) return (void*)shim_dgettext;
    if (!strcmp(nm,"bindtextdomain")) return (void*)shim_bindtextdomain;
    if (!strcmp(nm,"__duplocale")) return (void*)shim_duplocale;
    if (!strcmp(nm,"__nl_langinfo_l")) return (void*)shim_nl_langinfo_l;
    if (!strcmp(nm,"__strftime_l")) return (void*)shim_strftime_l;
    if (!strcmp(nm,"__strxfrm_l")) return (void*)shim_strxfrm_l;
    if (!strcmp(nm,"__strtof_l")) return (void*)shim_strtof_l;
    if (!strcmp(nm,"_dl_find_object")) return (void*)shim_dl_find_object;
    if (!strcmp(nm,"pthread_mutex_lock")) return (void*)shim_mutex_lock;
    if (!strcmp(nm,"pthread_mutex_trylock")) return (void*)shim_mutex_trylock;
    if (!strcmp(nm,"pthread_mutex_unlock")) return (void*)shim_mutex_unlock;
    if (!strcmp(nm,"pthread_mutex_init")) return (void*)shim_mutex_init;
    if (!strcmp(nm,"pthread_mutex_destroy")) return (void*)shim_mutex_destroy;
    if (!strcmp(nm,"pthread_mutexattr_init")) return (void*)shim_mutexattr_init;
    if (!strcmp(nm,"pthread_mutexattr_destroy")) return (void*)shim_mutexattr_destroy;
    if (!strcmp(nm,"pthread_mutexattr_settype")) return (void*)shim_mutexattr_settype;
    if (!strcmp(nm,"pthread_mutexattr_gettype")) return (void*)shim_mutexattr_gettype;
    if (!strcmp(nm,"pthread_condattr_init")) return (void*)shim_condattr_init;
    if (!strcmp(nm,"pthread_condattr_destroy")) return (void*)shim_condattr_destroy;
    if (!strcmp(nm,"pthread_condattr_setclock")) return (void*)shim_condattr_setclock;
    if (!strcmp(nm,"pthread_condattr_getclock")) return (void*)shim_condattr_getclock;
    if (!strcmp(nm,"pthread_cond_init")) return (void*)shim_cond_init;
    if (!strcmp(nm,"pthread_cond_destroy")) return (void*)shim_cond_destroy;
    if (!strcmp(nm,"pthread_cond_signal")) return (void*)shim_cond_signal;
    if (!strcmp(nm,"pthread_cond_broadcast")) return (void*)shim_cond_broadcast;
    if (!strcmp(nm,"pthread_cond_wait")) return (void*)shim_cond_wait;
    if (!strcmp(nm,"pthread_cond_timedwait")) return (void*)shim_cond_timedwait;
    if (!strcmp(nm,"__ctype_b_loc")) return (void*)shim_ctype_b_loc;
    if (!strcmp(nm,"__ctype_tolower_loc")) return (void*)shim_ctype_tolower_loc;
    if (!strcmp(nm,"__ctype_toupper_loc")) return (void*)shim_ctype_toupper_loc;
    if (!strcmp(nm,"__xpg_basename")) return (void*)shim_xpg_basename;
    if (!strcmp(nm,"__printf_chk")) return (void*)shim_printf_chk;
    if (!strcmp(nm,"__vfprintf_chk")) return (void*)shim_vfprintf_chk;
    if (!strcmp(nm,"__vswprintf_chk")) return (void*)shim_vswprintf_chk;
    if (!strcmp(nm,"__longjmp_chk")) return (void*)shim_longjmp_chk;
    if (!strcmp(nm,"__mbstowcs_chk")) return (void*)shim_mbstowcs_chk;
    if (!strcmp(nm,"__wcsncpy_chk")) return (void*)shim_wcsncpy_chk;
    if (!strcmp(nm,"__realpath_chk")) return (void*)shim_realpath_chk;
    if (!strcmp(nm,"mallinfo2")) return (void*)shim_mallinfo2;
    if (!strcmp(nm,"__isoc99_sscanf")) return (void*)shim_isoc99_sscanf;
    if (!strcmp(nm,"__isoc99_scanf")) return (void*)scanf;
    if (!strcmp(nm,"__isoc23_sscanf")) return (void*)shim_isoc99_sscanf;
    if (!strcmp(nm,"__stack_chk_fail")) return (void*)shim_stack_chk_fail;
    if (!strcmp(nm,"__stack_chk_fail_local")) return (void*)shim_stack_chk_fail;
    if (!strcmp(nm,"program_invocation_short_name")) return (void*)&g_prog_short_name;
    if (!strcmp(nm,"program_invocation_name")) return (void*)&g_prog_full_name;
    return NULL;
}

/* ---------- fault handler ---------- */
static volatile int g_phase;
static volatile const char *g_phase_text = "?";
static int mem_readable(unsigned long p) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512]; unsigned long a, b; int ok = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%lx-%lx", &a, &b) == 2 && p >= a && p < b) {
            ok = strstr(line, " r") != NULL;
            break;
        }
    }
    fclose(f);
    return ok;
}

static void dump_stack(unsigned long rsp, int max) {
    fprintf(stderr, "  --- top of stack (rsp[0] is the faulting call's return address) ---\n");
    for (int i = 0; i < 8; i++) {
        if (!mem_readable(rsp + i * 8)) break;
        unsigned long v = *(unsigned long *)(rsp + i * 8);
        Dl_info di; memset(&di, 0, sizeof di);
        if (dladdr((void*)v, &di) && di.dli_fname) {
            const char *nm = strrchr(di.dli_fname, '/');
            fprintf(stderr, "  rsp[%d] = %p  %s+0x%lx\n", i, (void*)v, nm ? nm + 1 : di.dli_fname,
                    (unsigned long)v - (unsigned long)di.dli_fbase);
        } else {
            fprintf(stderr, "  rsp[%d] = %p\n", i, (void*)v);
        }
    }
    fprintf(stderr, "  --- stack scan (returns into loaded modules) ---\n");
    unsigned long *p = (unsigned long *)rsp;
    int shown = 0;
    for (int i = 0; i < 16384 && shown < max; i++) {
        if (!mem_readable((unsigned long)&p[i])) break;
        unsigned long v = p[i];
        if (v < 0x100000) continue;
        Dl_info di; memset(&di, 0, sizeof di);
        if (dladdr((void*)v, &di) && di.dli_fname) {
            const char *nm = strrchr(di.dli_fname, '/');
            fprintf(stderr, "  [%4d] %p  %s+0x%lx\n", i, (void*)v, nm ? nm + 1 : di.dli_fname,
                    (unsigned long)v - (unsigned long)di.dli_fbase);
            shown++;
        }
    }
}

/* 把我们模块/bionic 都解析成人能看的名字 */
static void fmt_addr(unsigned long v, char *buf, size_t n) {
    for (int k = 0; k < g_nlibs; k++) {
        unsigned long b = (unsigned long)g_libs[k].base;
        if (v >= b && v < b + g_libs[k].mapsize) {
            const char *nm = strrchr(g_libs[k].name, '/');
            snprintf(buf, n, "%s+0x%lx", nm ? nm + 1 : g_libs[k].name, v - b);
            return;
        }
    }
    Dl_info di; memset(&di, 0, sizeof di);
    if (dladdr((void *)v, &di) && di.dli_fname && di.dli_fbase) {
        const char *nm = strrchr(di.dli_fname, '/');
        snprintf(buf, n, "%s+0x%lx", nm ? nm + 1 : di.dli_fname, v - (unsigned long)di.dli_fbase);
        return;
    }
    buf[0] = 0;
}

/* 打印从 rsp 开始的栈，标出落在我们模块里的返回地址，以及像 HRESULT 的值 */
static void dump_stack_all(unsigned long rsp, int max) {
    fprintf(stderr, "  --- stack scan from %p ---\n", (void *)rsp);
    unsigned long *p = (unsigned long *)rsp;
    int shown = 0;
    for (int i = 0; i < 20000; i++) {
        if (!mem_readable((unsigned long)&p[i])) break;
        unsigned long v = p[i];
        char nm[160];
        fmt_addr(v, nm, sizeof nm);
        if (nm[0] && shown < max) {
            fprintf(stderr, "  [%5d] %016lx  %s\n", i, v, nm);
            shown++;
            continue;
        }
        /* HRESULT / NTSTATUS 形状：0x8007xxxx 0x887Axxxx 0xC000xxxx 0x8899xxxx */
        if ((v >> 32) == 0) {
            unsigned long hi = (v >> 16) & 0xffff;
            if (hi == 0x8007 || hi == 0x887a || hi == 0xc000 || hi == 0x8899 || hi == 0x887b || hi == 0x8000)
                fprintf(stderr, "  [%5d] %016lx  <-- HRESULT/NTSTATUS? 0x%08lx\n", i, v, v);
        }
    }
}

static inline unsigned long read_rsp(void) {
    unsigned long r;
    __asm__ __volatile__("movq %%rsp, %0" : "=r"(r));
    return r;
}

static void shim_abort(void) {
    fprintf(stderr, "\n[abort] abort() called\n");
    dump_stack_all(read_rsp(), 60);
    fflush(stderr);
    _exit(134);
}

static int wr_hex(char *b, int n, unsigned long v) {
    char t[16]; int m = 0;
    if (!v) t[m++] = '0';
    while (v) { unsigned d = (unsigned)(v & 0xf); t[m++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v >>= 4; }
    while (m) b[n++] = t[--m];
    return n;
}

/* handler 里可用的最小栈回溯：只用 write(2)，不碰 stdio/malloc */
/* 崩溃的 logcat 通道（g_logw 在 segv 附近定义；栈帧打印也要发一份） */
static int (*g_logw)(int, const char *, const char *);

static void dump_stack_fast(unsigned long rsp, int max) {
    for (int i = 0; i < max; i++) {
        unsigned long v = ((unsigned long *)rsp)[i];
        if (!v) continue;
        char c[192]; int k = 0;
        const char *pre = "  #";
        while (*pre) c[k++] = *pre++;
        { char t[24]; int m = 0; unsigned long x = (unsigned long)i;
          if (!x) t[m++] = '0';
          while (x) { unsigned d = (unsigned)(x & 0xf); t[m++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); x >>= 4; }
          while (m) c[k++] = t[--m]; }
        c[k++] = ' '; c[k++] = '0'; c[k++] = 'x';
        { char t[24]; int m = 0; unsigned long x = v;
          if (!x) t[m++] = '0';
          while (x) { unsigned d = (unsigned)(x & 0xf); t[m++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); x >>= 4; }
          while (m) c[k++] = t[--m]; }
        for (int j = 0; j < g_nlibs; j++) {
            unsigned long bb = (unsigned long)g_libs[j].base;
            if (v >= bb && v < bb + g_libs[j].mapsize) {
                c[k++] = ' '; c[k++] = '<'; c[k++] = ' '; 
                const char *nm = g_libs[j].name; int L = 0; while (nm[L]) L++;
                for (int q = 0; q < L && k < 150; q++) c[k++] = nm[q];
                c[k++] = '+'; c[k++] = '0'; c[k++] = 'x';
                { char t2[24]; int m2 = 0; unsigned long x2 = v - bb;
                  if (!x2) t2[m2++] = '0';
                  while (x2) { unsigned d = (unsigned)(x2 & 0xf); t2[m2++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); x2 >>= 4; }
                  while (m2) c[k++] = t2[--m2]; }
                c[k++] = ' '; c[k++] = '>'; c[k++] = ' ';
                break;
            }
        }
        c[k++] = '\n'; c[k] = 0;
        syscall(SYS_write, 2, c, (unsigned long)k);
        if (g_logw) g_logw(6 /*ERROR*/, "d3d12ldr", c);   /* 栈帧也进 logcat */
    }
}

/* ---- 崩溃报告的第二条通道：logcat ----
 * 系统服务（hwcomposer / SF / systemui / app）的 stderr 是 /dev/null，
 * 我们的 segv handler 只写 fd 2，结果只能看到 init 的 "exited with status 139"。
 * 入口处 dlopen("liblog.so") 一次把 __android_log_write 拿到，handler 里只调函数指针。 */
typedef int (*ldr_logw_t)(int, const char *, const char *);
static ldr_logw_t g_logw;
/* 用 /proc/self/maps 反查地址属于哪个模块。崩溃 handler 里只认我们自己 mmap 的库，
 * 落在 bionic / libhwui / libskia 里的 rip 会显示 NOT-OURS，看不出是哪一步。
 * 只用 syscall（open/read/close），静态缓冲避免大栈。 */
static char g_maps_buf[16384];
static int maps_lookup(unsigned long addr, char *out, int outsz) {
    int fd = (int)syscall(SYS_openat, -100 /*AT_FDCWD*/, "/proc/self/maps", 0 /*O_RDONLY*/, 0);
    if (fd < 0) return 0;
    long tot = (long)syscall(SYS_read, fd, g_maps_buf, sizeof(g_maps_buf) - 1);
    syscall(SYS_close, fd);
    if (tot <= 0) return 0;
    g_maps_buf[tot] = 0;
    char *p = g_maps_buf;
    while (*p) {
        char *eol = p; while (*eol && *eol != '\n') eol++;
        unsigned long s = 0, e = 0; char *q = p;
        while (*q && *q != '-') { char c = *q;
            if (c >= '0' && c <= '9') s = s * 16 + (unsigned long)(c - '0');
            else if (c >= 'a' && c <= 'f') s = s * 16 + (unsigned long)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') s = s * 16 + (unsigned long)(c - 'A' + 10);
            q++; }
        if (*q == '-') q++;
        while (*q && *q != ' ') { char c = *q;
            if (c >= '0' && c <= '9') e = e * 16 + (unsigned long)(c - '0');
            else if (c >= 'a' && c <= 'f') e = e * 16 + (unsigned long)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') e = e * 16 + (unsigned long)(c - 'A' + 10);
            q++; }
        if (addr >= s && addr < e) {
            int sp = 0; char *r = p;
            while (*r && *r != '\n') { if (*r == ' ') sp++; if (sp == 5) break; r++; }
            int k = 0;
            if (sp == 5) { r++; while (*r && *r != '\n' && k < outsz - 1) out[k++] = *r++; }
            out[k] = 0;
            return 1;
        }
        p = eol; if (*p == '\n') p++;
    }
    return 0;
}
static int ap_str(char *b, int n, const char *s) { while (s && *s) b[n++] = *s++; return n; }
static int ap_hex(char *b, int n, unsigned long v) {
    char t[20]; int m = 0;
    if (!v) t[m++] = '0';
    while (v) { unsigned d = (unsigned)(v & 0xf); t[m++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v >>= 4; }
    while (m) b[n++] = t[--m];
    return n;
}
static void ldr_logw_init(void) {
    if (g_logw) return;
    void *h = dlopen("liblog.so", RTLD_NOW);
    if (h) g_logw = (ldr_logw_t)dlsym(h, "__android_log_write");
}
static void segv(int sig, siginfo_t *si, void *uc) {
    { long tid = (long)syscall(SYS_gettid); char b[48]; int n = 0;
      b[n++] = '['; b[n++] = 'S'; b[n++] = 'E'; b[n++] = 'G'; b[n++] = 'V';
      b[n++] = ':'; b[n++] = 't'; b[n++] = '=';
      /* 简易十进制 */
      char tmp[16]; int m = 0; if (tid == 0) tmp[m++] = '0';
      while (tid > 0) { tmp[m++] = (char)('0' + tid % 10); tid /= 10; }
      while (m > 0) b[n++] = tmp[--m];
      b[n++] = ']'; b[n++] = '\n';
      syscall(SYS_write, 2, b, (unsigned long)n);
    }
    ucontext_t *u = uc;
    /* 快速路径：只用 write(2)，避免 handler 里碰 stdio/malloc 死锁 */
    {
        unsigned long rip_ = (unsigned long)u->uc_mcontext.gregs[REG_RIP];
        unsigned long addr_ = (unsigned long)si->si_addr;
        char b[128]; int n = 0;
        b[n++]='['; b[n++]='S'; b[n++]='I'; b[n++]='G'; b[n++]=']'; b[n++]=' ';
        b[n++]='r'; b[n++]='i'; b[n++]='p'; b[n++]='=';
        { unsigned long v = rip_; char t[16]; int m = 0; if (!v) t[m++]='0';
          while (v) { unsigned d = (unsigned)(v & 0xf); t[m++] = (char)(d<10?'0'+d:'a'+d-10); v >>= 4; }
          while (m) b[n++] = t[--m]; }
        b[n++]=' '; b[n++]='a'; b[n++]='d'; b[n++]='d'; b[n++]='r'; b[n++]='=';
        { unsigned long v = addr_; char t[16]; int m = 0; if (!v) t[m++]='0';
          while (v) { unsigned d = (unsigned)(v & 0xf); t[m++] = (char)(d<10?'0'+d:'a'+d-10); v >>= 4; }
          while (m) b[n++] = t[--m]; }
        b[n++]=' '; b[n++]='r'; b[n++]='a'; b[n++]='x'; b[n++]='=';
        { unsigned long v = (unsigned long)u->uc_mcontext.gregs[REG_RAX]; char t[16]; int m = 0; if (!v) t[m++]='0';
          while (v) { unsigned d = (unsigned)(v & 0xf); t[m++] = (char)(d<10?'0'+d:'a'+d-10); v >>= 4; }
          while (m) b[n++] = t[--m]; }
        b[n++]=' '; b[n++]='f'; b[n++]='s'; b[n++]='0'; b[n++]='=';
        { unsigned long v = 0; __asm__ __volatile__("movq %%fs:0, %0" : "=r"(v)); char t[16]; int m = 0; if (!v) t[m++]='0';
          while (v) { unsigned d = (unsigned)(v & 0xf); t[m++] = (char)(d<10?'0'+d:'a'+d-10); v >>= 4; }
          while (m) b[n++] = t[--m]; }
        b[n++]='\n'; b[n]=0;
        syscall(SYS_write, 2, b, (unsigned long)n);
        if (g_logw) g_logw(6 /*ERROR*/, "d3d12ldr", b);   /* 系统服务走 logcat 才看得见 */
        /* 再报一条带模块名+偏移的：只给裸 rip 没法判断是我们还是 Mesa/UMD */
        {
            char lb[192]; int k = 0, hit = -1;
            k = ap_str(lb, k, "RIP ");
            for (int i = 0; i < g_nlibs; i++) {
                unsigned long bb = (unsigned long)g_libs[i].base;
                if (rip_ >= bb && rip_ < bb + g_libs[i].mapsize) { hit = i; break; }
            }
            if (hit >= 0) {
                k = ap_str(lb, k, g_libs[hit].name);
                k = ap_str(lb, k, "+0x");
                k = ap_hex(lb, k, rip_ - (unsigned long)g_libs[hit].base);
            } else {
                /* 优先 dladdr：bionic 的库能给出 "库!函数名+偏移"，比 maps 只给路径有用得多 */
                Dl_info di;
                if (dladdr((void *)rip_, &di) && di.dli_fname) {
                    k = ap_str(lb, k, di.dli_fname);
                    if (di.dli_sname) {
                        k = ap_str(lb, k, "!");
                        k = ap_str(lb, k, di.dli_sname);
                    }
                    k = ap_str(lb, k, "+0x");
                    k = ap_hex(lb, k, rip_ - (unsigned long)di.dli_fbase);
                } else {
                    char mp[96];
                    if (maps_lookup(rip_, mp, sizeof mp) && mp[0]) {
                        k = ap_str(lb, k, mp);
                    } else {
                        k = ap_str(lb, k, "NOT-OURS/anon");
                    }
                    k = ap_str(lb, k, " rip=0x");
                    k = ap_hex(lb, k, rip_);
                }
            }
            k = ap_str(lb, k, " phase=");
            k = ap_str(lb, k, g_phase_text ? g_phase_text : "?");
            lb[k] = 0;
            if (g_logw) g_logw(6, "d3d12ldr", lb);
        }
        for (int i = 0; i < g_nlibs; i++) {
            unsigned long bb = (unsigned long)g_libs[i].base;
            if (rip_ >= bb && rip_ < bb + g_libs[i].mapsize) {
                char c[160]; int k = 0;
                c[k++]=' '; c[k++]=' '; c[k++]='i'; c[k++]='n'; c[k++]=' ';
                const char *nm = g_libs[i].name; int L = 0; while (nm[L]) L++;
                for (int j = 0; j < L && k < 120; j++) c[k++] = nm[j];
                c[k++]=' '; c[k++]='+'; c[k++]='0'; c[k++]='x';
                { unsigned long v = rip_ - bb; char t[16]; int m = 0; if (!v) t[m++]='0';
                  while (v) { unsigned d = (unsigned)(v & 0xf); t[m++] = (char)(d<10?'0'+d:'a'+d-10); v >>= 4; }
                  while (m) c[k++] = t[--m]; }
                c[k++]='\n'; c[k]=0;
                syscall(SYS_write, 2, c, (unsigned long)k);
                if (g_logw) g_logw(6, "d3d12ldr", c);
            }
        }
        dump_stack_fast((unsigned long)u->uc_mcontext.gregs[REG_RSP], 40);
        _exit(128 + sig);
    }
    unsigned long rip = 0, rsp = 0, rbp = 0, rax = 0, rdi = 0, rsi = 0, rbx = 0;
#ifdef __x86_64__
    rip = (unsigned long)u->uc_mcontext.gregs[REG_RIP];
    rsp = (unsigned long)u->uc_mcontext.gregs[REG_RSP];
    rbp = (unsigned long)u->uc_mcontext.gregs[REG_RBP];
    rax = (unsigned long)u->uc_mcontext.gregs[REG_RAX];
    rdi = (unsigned long)u->uc_mcontext.gregs[REG_RDI];
    rsi = (unsigned long)u->uc_mcontext.gregs[REG_RSI];
    rbx = (unsigned long)u->uc_mcontext.gregs[REG_RBX];
#endif
    fprintf(stderr, "\n[SIG%s] addr=%p rip=%p rsp=%p rbp=%p phase=%d(%s)\n",
            sig == SIGABRT ? "ABRT" : sig == SIGSEGV ? "SEGV" : sig == SIGBUS ? "BUS" :
            sig == SIGILL ? "ILL" : sig == SIGFPE ? "FPE" : "?",
            si->si_addr, (void*)rip, (void*)rsp, (void*)rbp, g_phase, g_phase_text);
    fprintf(stderr, "  rax=%p rbx=%p rdi=%p rsi=%p\n", (void*)rax, (void*)rbx, (void*)rdi, (void*)rsi);
#ifdef __x86_64__
    fprintf(stderr, "  r8=%p r9=%p r10=%p r12=%p r13=%p r14=%p r15=%p\n",
            (void*)u->uc_mcontext.gregs[REG_R8], (void*)u->uc_mcontext.gregs[REG_R9],
            (void*)u->uc_mcontext.gregs[REG_R10], (void*)u->uc_mcontext.gregs[REG_R12],
            (void*)u->uc_mcontext.gregs[REG_R13], (void*)u->uc_mcontext.gregs[REG_R14],
            (void*)u->uc_mcontext.gregs[REG_R15]);
#endif
    for (int i = 0; i < g_nlibs; i++) {
        unsigned long b = (unsigned long)g_libs[i].base;
        if (rip >= b && rip < b + g_libs[i].mapsize)
            fprintf(stderr, "  fault inside %s +0x%lx\n", g_libs[i].name, rip - b);
    }
    /* bytes at rip */
    if (rip) {
        if (mem_readable(rip)) {
            fprintf(stderr, "  bytes at rip:");
            for (int i = 0; i < 12; i++) fprintf(stderr, " %02x", ((unsigned char*)rip)[i]);
            fprintf(stderr, "\n");
        } else {
            fprintf(stderr, "  rip not readable (jumped to unmapped/garbage)\n");
        }
    }
    /* crude rbp chain */
    if (rax && mem_readable(rax)) {
        unsigned long *v = (unsigned long *)rax;
        fprintf(stderr, "  vtable/obj @ %p:", (void*)rax);
        for (int i = 0; i < 4; i++) fprintf(stderr, " %p", (void*)v[i]);
        fprintf(stderr, "\n");
        if (mem_readable(rax + 0xe8))
            fprintf(stderr, "  *(obj+0xe8) = %p\n", (void*)*(unsigned long *)(rax + 0xe8));
        if (mem_readable(rax + 0x10))
            fprintf(stderr, "  *(obj+0x10) = %p\n", (void*)*(unsigned long *)(rax + 0x10));
    }
    dump_stack(rsp, 25);
    unsigned long *fp = (unsigned long*)rbp;
    for (int i = 0; i < 8 && fp && mem_readable((unsigned long)fp); i++) {
        unsigned long ret = fp[1];
        const char *where = "?";
        long off = 0;
        for (int k = 0; k < g_nlibs; k++) {
            unsigned long b = (unsigned long)g_libs[k].base;
            if (ret >= b && ret < b + g_libs[k].mapsize) { where = g_libs[k].name; off = (long)(ret - b); }
        }
        fprintf(stderr, "  frame[%d] ret=%p (%s+0x%lx)\n", i, (void*)ret, where, (unsigned long)off);
        fp = (unsigned long*)fp[0];
    }
    _exit(139);
}

#define REL(l, off) ((void *)((char *)(l)->base + ((off) - (l)->minv)))

/* ---- 计时探针（TIMING=1 打开）：定位"一轮要 40–90 秒"到底花在哪 ---- */
static double g_t0;
static int g_timing;
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}
#define TLOG(...) do { if (g_timing) { printf("[T+%8.1fms] ", now_ms() - g_t0); printf(__VA_ARGS__); printf("\n"); } } while (0)
/* 环境变量"真值"判断：未设置 / 空串 / "0" 都算关。
 * getenv 返回 "" 也是非 NULL，直接写 if (getenv("X")) 会在空串时误开开关。 */
static int env_on(const char *n) { const char *v = getenv(n); return v && *v && *v != '0'; }

/* main() 和 glibc_ldr_init() 两条入口都要调一次，否则 .so 路径上没有任何计时输出 */
static void timing_begin(void) {
    g_t0 = now_ms();
    const char *tv = getenv("TIMING");
    g_timing = (tv && *tv && *tv != '0');
}

static void *find_symbol_in(lib_t *l, const char *name) {
    for (size_t i = 0; i < l->nsyms; i++)
        if (l->syms[i].st_name && l->syms[i].st_name < l->strsz &&
            l->syms[i].st_shndx != SHN_UNDEF &&
            !strcmp(l->strs + l->syms[i].st_name, name))
            return (void*)((char*)l->base + (l->syms[i].st_value - l->minv));
    return NULL;
}

/* ---- TLS 模拟（每线程） ----
 * 每个库的 PT_TLS 镜像按线程各分配一块，用 bionic 的 pthread_get/setspecific
 * 挂在线程上（初始内容从 PT_TLS 的 init image 拷）：
 *   __tls_get_addr(ti)   → 当前线程那块 + offset      （DTPMOD64 / DTPOFF64）
 *   TLSDESC 解析器       → (当前线程那块 + off) - tp    （gnu2 方言，调用方再 add %fs:0）
 * **initial-exec（R_X86_64_TPOFF64）没法模拟多线程**：偏移被烧进指令/GOT，
 * bionic 的 TCB 又不给匿名模块留静态槽位，只能让来源库改用全局动态模型
 * （Mesa 的 src/util/u_thread.h 已把 __THREAD_INITIAL_EXEC 降级成普通 thread_local）。 */
static inline void *fs_base_ptr(void) { return __builtin_thread_pointer(); }

#define TLS_MAX_MODS MAXLIBS
#include "tls-state.h"

/* ---- 热路径/解析器里的调试输出只能用裸 write(2) ----
 * tlsdesc_helper 会在 fprintf、malloc 等库函数【内部】被调用（它们自己也要解析 TLS）。
 * 在解析器里再 fprintf 就会重入 bionic 的 FILE 锁（非递归）→ 同一线程自锁，
 * 日志停在最后一行 [tlsdesc] 就是这个。所以这里全部绕开 stdio，并且默认关闭。 */
static void dbg_s(const char *s) { unsigned n = 0; while (s[n]) n++; if (n) syscall(__NR_write, 2, s, n); }
static void dbg_hex(unsigned long v) {
    char b[20]; int i = 19; b[19] = 0;
    if (!v) { dbg_s("0"); return; }
    while (v && i > 0) { int d = (int)(v & 15); b[--i] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v >>= 4; }
    dbg_s(b + i);
}
static void dbg_ptr(const void *p) { dbg_s("0x"); dbg_hex((unsigned long)p); }
static void dbg_dec(long v) {
    char b[24]; int i = 23; b[23] = 0;
    int neg = (v < 0); unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    if (!u) { dbg_s("0"); return; }
    while (u && i > 0) { b[--i] = (char)('0' + (u % 10)); u /= 10; }
    if (neg) b[--i] = '-';
    dbg_s(b + i);
}
static int tls_dbg_on(void) { static int v = -1; if (v < 0) v = env_on("TLS_DBG"); return v; }

static void *lib_tls_block(lib_t *l) {
    /* TLS_SHARED=1：退回"每模块一块共享镜像"（仅单线程安全），用于 A/B 对照 */
    static void *g_shared[MAXLIBS];
    static int shared_mode = -1;
    if (shared_mode < 0) { const char *e = getenv("TLS_SHARED"); shared_mode = (e && *e && *e != '0'); }
    if (shared_mode) {
        int i2 = (int)(l - g_libs);
        if (i2 < 0 || i2 >= MAXLIBS) return NULL;
        if (!g_shared[i2]) {
            size_t sz = l->tls_size ? ((l->tls_size + 0xfff) & ~(size_t)0xfff) : 0x1000;
            void *b = calloc(1, sz);
            { const char *ni = getenv("TLS_NOINIT"); if (!(ni && *ni && *ni != '0')) {
                  if (l->tls_init && l->tls_init_size) memcpy(b, l->tls_init, l->tls_init_size); } }
            g_shared[i2] = b;
            if (tls_dbg_on()) { dbg_s("  [tls] "); dbg_s(strrchr(l->name, '/') ? strrchr(l->name, '/') + 1 : l->name);
                dbg_s(" SHARED block="); dbg_ptr(b); dbg_s("\n"); }
        }
        return g_shared[i2];
    }
    struct tls_state *st = cur_tls_state();
    int idx = (int)(l - g_libs);
    if (!st || idx < 0 || idx >= TLS_MAX_MODS) {
        static int once2;
        if (!once2) { once2 = 1; dbg_s("[tls] lib_tls_block fail st="); dbg_ptr(st);
            dbg_s(" idx="); dbg_dec(idx); dbg_s("\n"); }
        return NULL;
    }
    if (!st->blocks[idx]) {
        size_t sz = l->tls_size ? ((l->tls_size + 0xfff) & ~(size_t)0xfff) : 0x1000;
        void *b = calloc(1, sz);
        if (!b) return NULL;
        { const char *ni = getenv("TLS_NOINIT"); if (!(ni && *ni && *ni != '0')) {
              if (l->tls_init && l->tls_init_size) memcpy(b, l->tls_init, l->tls_init_size); } }
        st->blocks[idx] = b;
        if (g_logw) { char lb[128]; int k = 0;
            k = ap_str(lb, k, "TLS ALLOC "); k = ap_str(lb, k, l->name);
            k = ap_str(lb, k, " tid="); k = ap_hex(lb, k, (unsigned long)syscall(SYS_gettid));
            lb[k] = 0; g_logw(4, "d3d12ldr", lb); }
        if (tls_dbg_on()) { dbg_s("  [tls] "); dbg_s(strrchr(l->name, '/') ? strrchr(l->name, '/') + 1 : l->name);
            dbg_s(" block="); dbg_ptr(b); dbg_s(" size="); dbg_hex(sz); dbg_s("\n"); }
    }
    return st->blocks[idx];
}

static void *tls_block_by_mod(int mod) {
    if (mod <= 0 || mod > MAXLIBS) return NULL;
    lib_t *l = &g_libs[mod - 1];
    if (!l->base) {
        static int once;
        if (!once) { once = 1; fprintf(stderr, "[tls] mod=%d has no base (g_nlibs=%d)\n", mod, g_nlibs); }
        return NULL;
    }
    return lib_tls_block(l);
}

static void r69_tls_sink_note(int mod, size_t off, size_t sz) {
    static int n;
    if (n >= 40) return;
    n++;
    if (!g_logw) return;
    char lb[192]; int k = 0;
    k = ap_str(lb, k, "TLS SINK mod="); k = ap_hex(lb, k, (unsigned long)mod);
    k = ap_str(lb, k, " off=0x"); k = ap_hex(lb, k, (unsigned long)off);
    k = ap_str(lb, k, " size=0x"); k = ap_hex(lb, k, (unsigned long)sz);
    k = ap_str(lb, k, " tid="); k = ap_hex(lb, k, (unsigned long)syscall(SYS_gettid));
    lb[k] = 0;
    g_logw(3, "d3d12ldr", lb);
}

static int shim_ioctl_probe(int fd, unsigned long req, ...) {
    va_list ap; va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    unsigned nr = (unsigned)(req & 0xffu);
    uint64_t snap[16];
    int nq = 0;
    if (arg && nr == 0x33u) nq = 12;
    else if (arg && nr == 0x3au) nq = 5;
    if (nq) { for (int i = 0; i < nq; i++) snap[i] = ((uint64_t *)arg)[i]; }
    int r = (int)syscall(SYS_ioctl, fd, (unsigned long)req, arg);
    if (nq && r >= 0 && g_logw) {
        static int n33c, n3ac;
        int *cnt = (nr == 0x33u) ? &n33c : &n3ac;
        if (*cnt < 400) {
            uint64_t *q = (uint64_t *)arg;
            for (int i = 0; i < nq && *cnt < 400; i++) {
                if (q[i] != snap[i]) {
                    (*cnt)++;
                    char lb[192]; int k = 0;
                    k = ap_str(lb, k, (nr == 0x33u) ? "CHG33 i=" : "CHG3A i=");
                    k = ap_hex(lb, k, (unsigned long)i);
                    k = ap_str(lb, k, " o="); k = ap_hex(lb, k, (unsigned long)snap[i]);
                    k = ap_str(lb, k, " n="); k = ap_hex(lb, k, (unsigned long)q[i]);
                    k = ap_str(lb, k, " tid="); k = ap_hex(lb, k, (unsigned long)syscall(SYS_gettid));
                    lb[k] = 0;
                    g_logw(4, "d3d12ldr", lb);
                }
            }
        }
    }
    return r;
}

/* TLSDESC 描述符 d[1] 里存 (module<<32 | offset)，解析时按当前线程算。 */
__attribute__((noinline, used)) static void *tlsdesc_helper(uint64_t arg) {
    int mod = (int)(arg >> 32);
    uint32_t off = (uint32_t)arg;
    void *blk = tls_block_by_mod(mod);
    static int dbg;
    if (dbg < 6 && tls_dbg_on()) {     /* 默认关：在解析器里 fprintf 会重入 FILE 锁自锁 */
        dbg++;
        dbg_s("[tlsdesc] arg="); dbg_hex((unsigned long)arg);
        dbg_s(" mod="); dbg_dec(mod);
        dbg_s(" off="); dbg_hex(off);
        dbg_s(" blk="); dbg_ptr(blk);
        dbg_s(" tp="); dbg_ptr(fs_base_ptr());
        dbg_s(" base="); dbg_ptr((mod > 0 && mod <= MAXLIBS) ? g_libs[mod - 1].base : (void *)0);
        dbg_s("\n");
    }
    if (!blk) blk = g_tls_sink;
    return (void *)((char *)blk + off - (char *)fs_base_ptr());
}

/* 调用序列 lea desc,%rax; call *(%rax)：描述符地址在 %rax，返回值是
 * 【相对 tp 的偏移】，调用方再 add %fs:0（bionic 的 %fs:0 也是 tp 自身）。
 *
 * 关键：TLSDESC 的 resolver 在 x86-64 TLS ABI 里被当成"只破坏 rax"的调用——
 * 调用点会把活值留在 rdi/rsi/rdx/rcx/r8-r11 甚至 xmm 里，指望 resolver 原样还回来
 * （glibc 自己的 __tlsdesc_dynamic 就是纯汇编 + push/pop）。
 * 我们之前直接 jmp 到 C 函数，这些寄存器全被冲掉：实测 Mesa 的
 * util_call_once_data_slow 里 rdi（= &once_flag）被冲成了我们的 pthread key
 * 0x80000001，于是 call_once(flag=0x80000001) → SIGSEGV 在 80000001。
 * 这里按 ABI 保存/恢复全部易失寄存器（GP + XMM），rax 作为返回值。 */
__attribute__((naked, used)) static void *shim_tlsdesc_resolver(void) {
    __asm__ __volatile__(
        "push %rdi\n\tpush %rsi\n\tpush %rdx\n\tpush %rcx\n\t"
        "push %r8\n\tpush %r9\n\tpush %r10\n\tpush %r11\n\t"
        "sub $264, %rsp\n\t"
        "movups %xmm0, 0(%rsp)\n\tmovups %xmm1, 16(%rsp)\n\tmovups %xmm2, 32(%rsp)\n\tmovups %xmm3, 48(%rsp)\n\t"
        "movups %xmm4, 64(%rsp)\n\tmovups %xmm5, 80(%rsp)\n\tmovups %xmm6, 96(%rsp)\n\tmovups %xmm7, 112(%rsp)\n\t"
        "movups %xmm8, 128(%rsp)\n\tmovups %xmm9, 144(%rsp)\n\tmovups %xmm10, 160(%rsp)\n\tmovups %xmm11, 176(%rsp)\n\t"
        "movups %xmm12, 192(%rsp)\n\tmovups %xmm13, 208(%rsp)\n\tmovups %xmm14, 224(%rsp)\n\tmovups %xmm15, 240(%rsp)\n\t"
        "movq 8(%rax), %rdi\n\t"
        "call tlsdesc_helper\n\t"
        "movups 0(%rsp), %xmm0\n\tmovups 16(%rsp), %xmm1\n\tmovups 32(%rsp), %xmm2\n\tmovups 48(%rsp), %xmm3\n\t"
        "movups 64(%rsp), %xmm4\n\tmovups 80(%rsp), %xmm5\n\tmovups 96(%rsp), %xmm6\n\tmovups 112(%rsp), %xmm7\n\t"
        "movups 128(%rsp), %xmm8\n\tmovups 144(%rsp), %xmm9\n\tmovups 160(%rsp), %xmm10\n\tmovups 176(%rsp), %xmm11\n\t"
        "movups 192(%rsp), %xmm12\n\tmovups 208(%rsp), %xmm13\n\tmovups 224(%rsp), %xmm14\n\tmovups 240(%rsp), %xmm15\n\t"
        "add $264, %rsp\n\t"
        "pop %r11\n\tpop %r10\n\tpop %r9\n\tpop %r8\n\t"
        "pop %rcx\n\tpop %rdx\n\tpop %rsi\n\tpop %rdi\n\t"
        "ret\n\t");
}

static lib_t *tls_sym_lookup(lib_t *l, Elf64_Sym *tsym, uint64_t *tv) {
    *tv = 0;
    if (tsym && tsym->st_shndx != SHN_UNDEF) { *tv = tsym->st_value; return l; }
    if (!tsym || !tsym->st_name || tsym->st_name >= l->strsz) return l;
    const char *nm = l->strs + tsym->st_name;
    for (int k = 0; k < g_nlibs; k++) {
        if (&g_libs[k] == l) continue;
        for (size_t j = 0; j < g_libs[k].nsyms; j++) {
            if (g_libs[k].syms[j].st_shndx == SHN_UNDEF) continue;
            if (g_libs[k].syms[j].st_name && g_libs[k].syms[j].st_name < g_libs[k].strsz &&
                !strcmp(g_libs[k].strs + g_libs[k].syms[j].st_name, nm)) {
                *tv = g_libs[k].syms[j].st_value;
                return &g_libs[k];
            }
        }
    }
    return l;
}

static void apply_lib(lib_t *l, Elf64_Rela *rela, size_t n, const char *seg) {
    /* 调试开关的 getenv 全部提到循环外：一方面 39 万条重定位时 getenv 本身不便宜，
     * 另一方面 getenv("X") 返回 "" 也是非 NULL——早期版本因此让 strstr(nm, "") 恒命中，
     * 每条重定位都 fprintf，光 UMD 就多花 1.3 秒。（HANDOFF 坑 8 的同一个坑） */
    const char *lw = getenv("LOG_WEAK");  if (lw && !*lw) lw = NULL;
    const char *ws = getenv("WEAK_STUB"); if (ws && !*ws) ws = NULL;
    for (size_t i = 0; i < n; i++) {
        uint32_t type = ELF64_R_TYPE(rela[i].r_info);
        uint32_t symi = ELF64_R_SYM(rela[i].r_info);
        uint64_t *loc = (uint64_t*)((char*)l->base + (rela[i].r_offset - l->minv));
        if (type == R_X86_64_RELATIVE) {
            *loc = (uint64_t)((char*)l->base + (rela[i].r_addend - l->minv));
            continue;
        }
        /* TLS 重定位。模块号必须是【定义该 TLS 变量的库】的序号，不是发起者自己的：
         * 否则跨库 TLS（典型：UMD 写 std::__once_call、libstdc++ 读）会落在两个不同槽里。 */
        if (type == R_X86_64_DTPMOD64 || type == R_X86_64_DTPOFF64) {
            Elf64_Sym *tsym = (symi < l->nsyms) ? &l->syms[symi] : NULL;
            const char *tnm = (tsym && tsym->st_name < l->strsz) ? l->strs + tsym->st_name : NULL;
            int mod = (int)(l - g_libs) + 1;
            uint64_t off = 0;
            if (tsym && tsym->st_shndx != SHN_UNDEF) {
                off = tsym->st_value;
            } else if (tnm) {
                for (int k = 0; k < g_nlibs; k++) {
                    if (&g_libs[k] == l) continue;
                    for (size_t j = 0; j < g_libs[k].nsyms; j++) {
                        if (g_libs[k].syms[j].st_shndx == SHN_UNDEF) continue;
                        if (g_libs[k].syms[j].st_name && g_libs[k].syms[j].st_name < g_libs[k].strsz &&
                            !strcmp(g_libs[k].strs + g_libs[k].syms[j].st_name, tnm)) {
                            mod = k + 1;
                            off = g_libs[k].syms[j].st_value;
                            goto tls_done;
                        }
                    }
                }
            }
        tls_done:
            *loc = (type == R_X86_64_DTPMOD64) ? (uint64_t)mod : (off + rela[i].r_addend);
            continue;
        }
        if (type == R_X86_64_TPOFF64) {
            Elf64_Sym *tsym = (symi < l->nsyms) ? &l->syms[symi] : NULL;
            uint64_t tv = 0;
            lib_t *def = tls_sym_lookup(l, tsym, &tv);
            void *blk = lib_tls_block(def);
            long tpoff = (long)((char *)blk - (char *)fs_base_ptr());
            *loc = (uint64_t)(tpoff + (long)tv + (long)rela[i].r_addend);
            printf("  [tls] %s TPOFF64 %s off=0x%lx tpoff=%ld (initial-exec: 仅加载线程有效)\n",
                   strrchr(l->name, '/') ? strrchr(l->name, '/') + 1 : l->name,
                   (tsym && tsym->st_name < l->strsz) ? l->strs + tsym->st_name : "?",
                   (unsigned long)tv, tpoff);
            continue;
        }
        if (type == 36 /* R_X86_64_TLSDESC */) {
            Elf64_Sym *tsym = (symi < l->nsyms) ? &l->syms[symi] : NULL;
            uint64_t tv = 0;
            lib_t *def = tls_sym_lookup(l, tsym, &tv);
            int mod = (int)(def - g_libs) + 1;
            uint32_t off = (uint32_t)(tv + (uint64_t)rela[i].r_addend);
            uint64_t *d = loc;
            d[0] = (uint64_t)(uintptr_t)&shim_tlsdesc_resolver;
            d[1] = ((uint64_t)(uint32_t)mod << 32) | (uint64_t)off;
            printf("  [tls] %s TLSDESC %s mod=%d off=0x%x (dynamic)\n",
                   strrchr(l->name, '/') ? strrchr(l->name, '/') + 1 : l->name,
                   (tsym && tsym->st_name < l->strsz) ? l->strs + tsym->st_name : "?", mod, off);
            continue;
        }
        if (type != R_X86_64_GLOB_DAT && type != R_X86_64_JUMP_SLOT && type != R_X86_64_64)
            continue;
        Elf64_Sym *sym = (symi < l->nsyms) ? &l->syms[symi] : NULL;
        const char *nm = (sym && sym->st_name < l->strsz) ? l->strs + sym->st_name : NULL;
        uint64_t val = 0;
        if (sym && sym->st_shndx != SHN_UNDEF) {
            val = (uint64_t)((char*)l->base + (sym->st_value - l->minv));
        } else {
            void *sa = shim_lookup(nm);
            if (!sa && nm) {
                for (int k = 0; k < g_nlibs; k++) {
                    if (&g_libs[k] == l) continue;
                    void *r = find_symbol_in(&g_libs[k], nm);
                    if (r && (unsigned long)r >= (unsigned long)g_libs[k].base &&
                        (unsigned long)r < (unsigned long)g_libs[k].base + g_libs[k].mapsize) {
                        sa = r; break;
                    }
                }
            }
            if (!sa) { sa = (nm && nm[0]) ? dlsym(RTLD_DEFAULT, nm) : NULL; }
            if (sa) {
                val = (uint64_t)sa;
            } else if (sym && ELF64_ST_BIND(sym->st_info) == STB_WEAK) {
                if (lw && nm)
                    fprintf(stderr, "  [weak] %s: %s -> 0 (loc=%p)\n", l->name, nm, (void*)loc);
                /* 弱函数符号如果被无条件调用，置 0 会直接 rip=0。装个会说话的桩便于定位。 */
                if (ELF64_ST_TYPE(sym->st_info) == STT_FUNC && ws) {
                    extern long shim_weak_stub(void);
                    val = (uint64_t)(uintptr_t)&shim_weak_stub;
                } else {
                    val = 0;
                }
            } else {
                fprintf(stderr, "  UNRESOLVED[%s/%s]: %s\n", l->name, seg, nm ? nm : "(null)");
                continue;
            }
        }
        *loc = val + (type == R_X86_64_64 ? rela[i].r_addend : 0);
    }
}

static void *load_so(const char *path);
/* All relocation destinations in the qualified builds are writable. Match
 * the exact load layout as well as the build ID: the mapping loop below
 * assumes that each non-writable segment starts on a separate 4 KiB page. */
static int share_elf_pages(const char *path, const void *map, size_t size,
                           const Elf64_Ehdr *eh, const Elf64_Phdr *ph) {
    static const struct shared_elf {
        const char *name;
        size_t size, note_offset;
        unsigned phnum;
        unsigned char id[20];
        struct { uint64_t off, va, file, mem; unsigned flags; } seg[4];
    } builds[] = {
        {"amdxc64.so", 177214696, 0x300, 12,
         {0xb4,0x85,0x98,0x58,0x9f,0x6d,0x59,0x45,0xe7,0x98,
          0x55,0xe3,0x24,0x79,0x18,0xb8,0x78,0x4c,0x3d,0x26},
         {{0,0,0x9026b0,0x9026b0,PF_R},
          {0x903000,0x903000,0x4b40e9d,0x4b40e9d,PF_R|PF_X},
          {0x5444000,0x5444000,0x49bcc71,0x49bcc71,PF_R},
          {0x9e00cf0,0x9e01cf0,0xaffe70,0xbf8b70,PF_R|PF_W}}},
        {"libcrypto.so.3", 6386528, 0x2e0, 12,
         {0x0e,0xbc,0xfe,0xae,0xe2,0x73,0xdc,0x86,0x0b,0x29,0xe0,0x67,0xd2,0x55,0xde,0xf7,0x04,0x18,0x5b,0x41},
         {{0x0,0x0,0xe3a00,0xe3a00,PF_R},
          {0xe4000,0xe4000,0x393f11,0x393f11,PF_R|PF_X},
          {0x478000,0x478000,0x11ec64,0x11ec64,PF_R},
          {0x5977b0,0x5977b0,0x7f2d8,0x82508,PF_R|PF_W}}},
        {"libstdc++.so.6", 2735712, 0x318, 13,
         {0x3a,0x7a,0x8c,0x6b,0xd9,0x22,0xe6,0x31,0x4e,0xdb,0x7a,0x71,0x0f,0xdc,0x02,0x93,0x26,0x5c,0x2c,0x42},
         {{0x0,0x0,0xa2968,0xa2968,PF_R},
          {0xa3000,0xa3000,0x15a371,0x15a371,PF_R|PF_X},
          {0x1fe000,0x1fe000,0x8be80,0x8be80,PF_R},
          {0x28a9f0,0x28a9f0,0x10930,0x14a50,PF_R|PF_W}}},
        {"libgallium-26.0.8.so", 22455544, 0x2e0, 12,
         {0xc7,0x22,0x46,0x30,0x09,0x32,0x49,0xc7,0x21,0x31,
          0x25,0xe5,0x90,0xde,0x87,0x43,0xd2,0x23,0x1d,0x18},
         {{0,0,0x7baf8,0x7baf8,PF_R},
          {0x7c000,0x7c000,0xa67871,0xa67871,PF_R|PF_X},
          {0xae4000,0xae4000,0x8bed28,0x8bed28,PF_R},
          {0x13a2d48,0x13a3d48,0x7b7b8,0xaca58,PF_R|PF_W}}}
    };
    const struct shared_elf *build = NULL;
    for (size_t i=0; i<sizeof(builds)/sizeof(builds[0]); i++)
        if (size == builds[i].size && !strcmp(libshort(path), builds[i].name)) {
            build = &builds[i]; break;
        }
    if (!build || eh->e_machine != EM_X86_64 || eh->e_phoff != sizeof(*eh) ||
        eh->e_phentsize != sizeof(*ph) || eh->e_phnum != build->phnum ||
        sysconf(_SC_PAGESIZE) != 4096) return 0;
    static const unsigned char header[] = {4,0,0,0,20,0,0,0,NT_GNU_BUILD_ID,0,0,0,'G','N','U',0};
    unsigned n=0; int found_id=0;
    for (int i=0; i<eh->e_phnum; i++) {
        if (ph[i].p_type == PT_NOTE && ph[i].p_offset == build->note_offset &&
            ph[i].p_filesz == sizeof(header)+sizeof(build->id)) {
            const char *note = (const char *)map + build->note_offset;
            found_id = !memcmp(note, header, sizeof(header)) &&
                       !memcmp(note+sizeof(header), build->id, sizeof(build->id));
        }
        if (ph[i].p_type != PT_LOAD) continue;
        if (n >= 4 || ph[i].p_offset != build->seg[n].off ||
            ph[i].p_vaddr != build->seg[n].va || ph[i].p_filesz != build->seg[n].file ||
            ph[i].p_memsz != build->seg[n].mem || ph[i].p_flags != build->seg[n].flags) return 0;
        n++;
    }
    return found_id && n == 4;
}

static lib_t *load_elf(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); return NULL; }
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < (off_t)sizeof(Elf64_Ehdr)) { close(fd); return NULL; }
    void *map = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) { perror("mmap"); close(fd); return NULL; }
    Elf64_Ehdr *eh = map;
    Elf64_Phdr *ph = (Elf64_Phdr*)((char*)map + eh->e_phoff);
    uint64_t minv = ~0ull, maxv = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_vaddr < minv) minv = ph[i].p_vaddr;
        if (ph[i].p_vaddr + ph[i].p_memsz > maxv) maxv = ph[i].p_vaddr + ph[i].p_memsz;
    }
    size_t mapsize = ((maxv - minv) + 0xfff) & ~(size_t)0xfff;
    void *base = mmap(NULL, mapsize, PROT_READ|PROT_WRITE|PROT_EXEC,
                      MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) { perror("mmap anon"); munmap(map, st.st_size); close(fd); return NULL; }
    int share = share_elf_pages(path, map, st.st_size, eh, ph);
    if (share) {
        for (int i=0; i<eh->e_phnum; i++) {
            if (ph[i].p_type != PT_LOAD || (ph[i].p_flags & PF_W)) continue;
            size_t len = ph[i].p_filesz & ~(size_t)4095;
            void *at = (char *)base + (ph[i].p_vaddr-minv);
            int prot = PROT_READ | ((ph[i].p_flags & PF_X) ? PROT_EXEC : 0);
            /* MAP_FIXED replaces only whole pages inside our new reservation. */
            if (mmap(at, len, prot, MAP_PRIVATE|MAP_FIXED, fd, ph[i].p_offset) == MAP_FAILED) {
                /* Discard even previously mapped segments (e.g. noexec mount).
                 * Nothing outside this function knows the base address yet. */
                munmap(base, mapsize);
                base = mmap(NULL, mapsize, PROT_READ|PROT_WRITE|PROT_EXEC,
                            MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
                if (base == MAP_FAILED) { munmap(map, st.st_size); close(fd); return NULL; }
                share = 0;
                break;
            }
        }
    }
    double _tcopy = now_ms();
    size_t _copied = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        size_t skip = share && !(ph[i].p_flags & PF_W) ? ph[i].p_filesz & ~(size_t)4095 : 0;
        size_t tail = ph[i].p_filesz - skip;
        if (tail) memcpy((char*)base + (ph[i].p_vaddr-minv) + skip,
                         (char*)map + ph[i].p_offset + skip, tail);
        _copied += tail;
    }
    TLOG("  [load] %s mmap+copy %.2f MB took %.1f ms", path, (double)_copied/1048576.0, now_ms() - _tcopy);
    Elf64_Dyn *dyn = NULL;
    void *eh_hdr = NULL;
    size_t tls_size = 0;
    const void *tls_init = NULL;
    size_t tls_init_size = 0;
    Elf64_Addr tls_vaddr = 0;
    Elf64_Xword tls_align = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) dyn = (Elf64_Dyn*)((char*)map + ph[i].p_offset);
        if (ph[i].p_type == 0x6474e550)  /* PT_GNU_EH_FRAME */
            eh_hdr = (char*)base + (ph[i].p_vaddr - minv);
        if (ph[i].p_type == PT_TLS) {
            tls_size = ph[i].p_memsz;
            tls_init = (char*)base + (ph[i].p_vaddr - minv);
            tls_init_size = ph[i].p_filesz;
            tls_vaddr = ph[i].p_vaddr; tls_align = ph[i].p_align;
        }
    }

    lib_t *l = &g_libs[g_nlibs];
    memset(l, 0, sizeof(*l));
    snprintf(l->name, sizeof(l->name), "%s", path);
    l->base = base; l->minv = minv; l->mapsize = mapsize; l->eh_hdr = eh_hdr;
    l->tls_size = tls_size;
    if (tls_size) fprintf(stderr, "  [tls] %s seg p_vaddr=0x%llx filesz=0x%llx memsz=0x%llx align=0x%llx\n",
                          strrchr(l->name, '/') ? strrchr(l->name, '/') + 1 : l->name,
                          (unsigned long long)tls_vaddr, (unsigned long long)tls_init_size,
                          (unsigned long long)tls_size, (unsigned long long)tls_align);
    l->tls_init = tls_init; l->tls_init_size = tls_init_size;
    /* 留一份原始 program headers：dl_iterate_phdr 注入时，unwinder 拿它算
     * PT_GNU_EH_FRAME 的运行时地址（p_vaddr + dlpi_addr）。 */
    l->phnum = eh->e_phnum;
    l->phdrs = (Elf64_Phdr *)malloc(sizeof(Elf64_Phdr) * (size_t)eh->e_phnum);
    if (l->phdrs) memcpy(l->phdrs, ph, sizeof(Elf64_Phdr) * (size_t)eh->e_phnum);

    uint64_t relasz=0, relaoff=0, jmprel=0, pltrelsz=0, symtab=0, strtab=0, strsz=0;
    for (Elf64_Dyn *d = dyn; d && d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_RELA: relaoff = d->d_un.d_ptr; break;
            case DT_RELASZ: relasz = d->d_un.d_val; break;
            case DT_JMPREL: jmprel = d->d_un.d_ptr; break;
            case DT_PLTRELSZ: pltrelsz = d->d_un.d_val; break;
            case DT_SYMTAB: symtab = d->d_un.d_ptr; break;
            case DT_STRTAB: strtab = d->d_un.d_ptr; break;
            case DT_STRSZ: strsz = d->d_un.d_val; break;
            case DT_INIT: l->init_fn = d->d_un.d_ptr; break;
            case DT_INIT_ARRAY: l->init_arr = d->d_un.d_ptr; break;
            case DT_INIT_ARRAYSZ: l->init_arrsz = d->d_un.d_val; break;
            default: break;
        }
    }
    l->syms = REL(l, symtab);
    l->strs = REL(l, strtab);
    l->strsz = strsz;
    /* 符号数量必须取 .dynsym 段的大小：(strtab - symtab)/24 在段顺序不连续时会多算，
     * 多出来的“幽灵符号”会被跨库查找命中，绑到垃圾地址（曾经把 getenv 绑成野指针）。 */
    l->nsyms = 0;
    if (eh->e_shoff && eh->e_shentsize) {
        Elf64_Shdr *sh = (Elf64_Shdr *)((char *)map + eh->e_shoff);
        for (int i = 0; i < eh->e_shnum; i++) {
            if (sh[i].sh_type == SHT_DYNSYM && sh[i].sh_entsize) {
                l->nsyms = sh[i].sh_size / sh[i].sh_entsize;
                break;
            }
        }
    }
    if (!l->nsyms && strtab > symtab) l->nsyms = (strtab - symtab) / sizeof(Elf64_Sym);
    g_nlibs++;
    printf("loaded %s base=%p size=0x%zx nsyms=%zu\n", path, base, mapsize, l->nsyms);
    /* 先加载 DT_NEEDED（只在我们自己的目录里找） */
    for (Elf64_Dyn *d = dyn; d && d->d_tag != DT_NULL; d++) {
        if (d->d_tag != DT_NEEDED) continue;
        const char *dep = l->strs + d->d_un.d_val;
        int have = 0;
        for (int k = 0; k < g_nlibs; k++) {
            const char *b = strrchr(g_libs[k].name, '/');
            if (b && !strcmp(b + 1, dep)) { have = 1; break; }
        }
        if (have) continue;
        char dp[512];
        int got = 0;
        const char *md = mesa_dir();
        if (md) {
            snprintf(dp, sizeof dp, "%s/%s", md, dep);
            if (access(dp, R_OK) == 0) { printf("  [dep] %s needs %s\n", path, dep); load_so(dp); got = 1; }
        }
        if (!got) {
            snprintf(dp, sizeof dp, "/data/local/tmp/wsl_drivers/%s", dep);
            if (access(dp, R_OK) == 0) { printf("  [dep] %s needs %s\n", path, dep); load_so(dp); }
        }
    }
    double _trel = now_ms();
    if (relasz) apply_lib(l, REL(l, relaoff), relasz / sizeof(Elf64_Rela), ".rela.dyn");
    if (pltrelsz) apply_lib(l, REL(l, jmprel), pltrelsz / sizeof(Elf64_Rela), ".rela.plt");
    TLOG("  [load] %s relocs %zu + %zu took %.1f ms", path, relasz / sizeof(Elf64_Rela), pltrelsz / sizeof(Elf64_Rela), now_ms() - _trel);
    /* dyn and section headers were the last users of the input view.
     * Runtime pointers refer to base (or the copied phdrs), not to map. */
    munmap(map, st.st_size);
    close(fd);
    return l;
}

/* 让 libgcc 的 unwinder 认识手动映射的模块：注册 .eh_frame */
static void register_unwind(lib_t *l) {
    if (!l->eh_hdr) return;
    static char store[MAXLIBS][256];
    unsigned char *hdr = (unsigned char *)l->eh_hdr;
    if (hdr[0] != 1) { printf("  [unwind] %s unknown eh_frame_hdr version %d\n", l->name, hdr[0]); return; }
    if (hdr[1] != 0x1b) { printf("  [unwind] %s enc=0x%02x unsupported\n", l->name, hdr[1]); return; }
    int32_t off;
    memcpy(&off, hdr + 4, 4);
    void *eh_frame = (void *)(hdr + 4 + off);
    void *sym = NULL;
    for (int i = 0; i < g_nlibs && !sym; i++) sym = find_symbol_in(&g_libs[i], "__register_frame_info");
    if (!sym) return;   /* libgcc 还没加载时静默跳过 */
    void (*reg)(void *, void *) = (void (*)(void *, void *))sym;
    reg(eh_frame, store[l - g_libs]);
    printf("  [unwind] %s registered eh_frame=%p\n", l->name, eh_frame);
}

/* SKIP_INIT="650,651" 之类的环境变量：跳过指定序号的 init（排查用） */
static int g_skip_list[64], g_skip_n;
static void load_skip_list(void) {
    const char *s = getenv("SKIP_INIT");
    if (!s) return;
    while (*s && g_skip_n < 64) {
        g_skip_list[g_skip_n++] = (int)strtol(s, (char **)&s, 10);
        if (*s == ',') s++;
    }
}
static int should_skip_init(size_t i) {
    for (int k = 0; k < g_skip_n; k++) if ((size_t)g_skip_list[k] == i) return 1;
    return 0;
}

/* glibc 的顺序应该是先 DT_INIT 再 DT_INIT_ARRAY，但实测我们这套加载器下
 * 反着来才稳定（GLIBC_INIT_ORDER=1 可切回 glibc 顺序做实验）。 */
static void run_inits(lib_t *l) {
    if (getenv("GLIBC_INIT_ORDER") && l->init_fn) {
        void (*fn)(void) = (void(*)(void))((char*)l->base + (l->init_fn - l->minv));
        if (getenv("SKIP_DT_INIT")) {
            printf("  %s DT_INIT = %p SKIPPED\n", l->name, (void*)fn);
        } else {
            const char *dv = getenv("DUMP_VADDR");
            if (dv) {
                unsigned long want = strtoul(dv, NULL, 0);
                if (want >= l->minv && want < l->minv + l->mapsize) {   /* 只在该库映射范围内读 */
                    uint64_t v = *(uint64_t *)((char*)l->base + (want - l->minv));
                    printf("  [dump] *(lib+0x%lx) = 0x%lx\n", want, (unsigned long)v);
                }
            }
            printf("  %s DT_INIT = %p\n", l->name, (void*)fn);
            g_phase_text = "DT_INIT";
            g_phase = 900;
            fn();
            printf("  %s DT_INIT ok\n", l->name);
        }
    }
    if (l->init_arr && l->init_arrsz) {
        size_t n = l->init_arrsz / sizeof(uint64_t);
        uint64_t *arr = REL(l, l->init_arr);
        printf("  %s init_array: %zu entries\n", l->name, n);
        for (size_t i = 0; i < n; i++) {
            if (!arr[i]) { printf("  init[%zu] = (null)\n", i); continue; }
            if (should_skip_init(i)) { printf("  init[%zu] = %p ... SKIPPED\n", i, (void*)arr[i]); continue; }
            void (*fn)(void) = (void(*)(void))arr[i];
            printf("  init[%zu] = %p ...\n", i, (void*)fn);
            static char buf[128];
            snprintf(buf, sizeof buf, "init[%zu] of %s", i, l->name);
            g_phase_text = buf;
            g_phase = 100 + (int)i;
            double _ti = now_ms();
            fn();
            TLOG("  init[%zu] of %s took %.1f ms", i, l->name, now_ms() - _ti);
            printf("  init[%zu] ok\n", i);
        }
    }
    if (!getenv("GLIBC_INIT_ORDER") && l->init_fn) {
        void (*fn)(void) = (void(*)(void))((char*)l->base + (l->init_fn - l->minv));
        if (getenv("SKIP_DT_INIT")) {
            printf("  %s DT_INIT = %p SKIPPED\n", l->name, (void*)fn);
        } else {
            const char *dv = getenv("DUMP_VADDR");
            if (dv) {
                unsigned long want = strtoul(dv, NULL, 0);
                if (want >= l->minv && want < l->minv + l->mapsize) {   /* 只在该库映射范围内读 */
                    uint64_t v = *(uint64_t *)((char*)l->base + (want - l->minv));
                    printf("  [dump] *(lib+0x%lx) = 0x%lx\n", want, (unsigned long)v);
                }
            }
            printf("  %s DT_INIT = %p\n", l->name, (void*)fn);
            g_phase_text = "DT_INIT";
            g_phase = 900;
            fn();
            printf("  %s DT_INIT ok\n", l->name);
        }
    }
}

/* 容器里没有 /usr/lib/wsl，驱动 UMD 由 run.sh 拷到 /data/local/tmp/wsl_drivers/ 下，
 * 这里把绝对路径重写过去。 */
static const char *map_driver_path(const char *path, char *buf, size_t bufsz) {
    const char *tag = "/usr/lib/wsl/drivers/";
    if (strncmp(path, tag, strlen(tag)) == 0) {
        const char *rest = path + strlen(tag);
        const char *slash = strrchr(rest, '/');
        if (slash) {
            snprintf(buf, bufsz, "/data/local/tmp/wsl_drivers/%s", slash + 1);
            return buf;
        }
    }
    /* Mesa 的 EGL 会用默认搜索路径拼出 /usr/lib/x86_64-linux-gnu/dri/xxx_dri.so，
     * 容器里没有这些目录，统一重定向到 MESA_DIR。 */
    if (strstr(path, "/dri/")) {
        const char *base = strrchr(path, '/');
        if (base) {
            snprintf(buf, bufsz, "%s/%s", mesa_dir(), base + 1);
            return buf;
        }
    }
    return path;
}

static void *load_so(const char *path);

/* 只比 basename：core 传来的是 /usr/lib/wsl/drivers/<inf>/amdxc64.so，
 * 已加载的名字是 /data/local/tmp/wsl_drivers/amdxc64.so，路径不同但同一个库。
 * 用 strstr 判重会把同一个 177MB 的 UMD 再 load 一份，第二份初始化直接把进程搞崩。 */
static const char *base_of(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}
static void *shim_dlopen(const char *path, int flags) {
    (void)flags;
    if (!path) return NULL;
    for (int i = 0; i < g_nlibs; i++)
        if (!strcmp(base_of(g_libs[i].name), base_of(path))) {
            printf("  [dlopen] %s -> already loaded (%s)\n", path, g_libs[i].name);
            return &g_libs[i];
        }
    char buf[512];
    const char *real = map_driver_path(path, buf, sizeof buf);
    /* 裸名字（libssl.so.3 之类）先去驱动目录找 */
    if (!strchr(path, '/') && real == path) {
        snprintf(buf, sizeof buf, "/data/local/tmp/%s", path);
        if (access(buf, R_OK) == 0) real = buf;
        const char *md = mesa_dir();
        if (real == path && md) {
            snprintf(buf, sizeof buf, "%s/%s", md, path);
            if (access(buf, R_OK) == 0) real = buf;
        }
        if (real == path) {
            snprintf(buf, sizeof buf, "/data/local/tmp/wsl_drivers/%s", path);
            if (access(buf, R_OK) == 0) real = buf;
        }
    }
    printf("  [dlopen] %s%s%s\n", path, real != path ? " -> " : "", real != path ? real : "");
    return load_so(real);
}
static void *shim_dlsym(void *handle, const char *name) {
    if (!handle) { printf("  [dlsym] NULL handle, asked for %s\n", name ? name : "(null)"); return NULL; }
    if (handle == RTLD_DEFAULT || handle == (void*)RTLD_NEXT) return dlsym(RTLD_DEFAULT, name);
    const char *base = strrchr(((lib_t*)handle)->name, '/');
    base = base ? base + 1 : ((lib_t*)handle)->name;
    void *r = find_symbol_in((lib_t*)handle, name);
    if (!r) {
        /* 真实链接器有全局符号作用域：别的已加载库里有也算找到（MD5_Init 在 libcrypto 里） */
        for (int k = 0; k < g_nlibs && !r; k++) {
            if ((void*)&g_libs[k] == handle) continue;
            r = find_symbol_in(&g_libs[k], name);
            if (r) printf("  [dlsym] %s not in %s, found in %s\n", name, base,
                          strrchr(g_libs[k].name, '/') ? strrchr(g_libs[k].name, '/') + 1 : g_libs[k].name);
        }
    }
    if (!r) {
        printf("  [dlsym] MISS %s in %s\n", name, base);
        r = dlsym(RTLD_DEFAULT, name);
        printf("  [dlsym]   fallback(RTLD_DEFAULT) -> %p\n", r);
    } else {
        printf("  [dlsym] %s in %s -> %p\n", name, base, r);
    }
    return r;
}


/* 任意 glibc .so 的加载入口（dlopen shim 用） */
static int g_skip_cxx_init;
static void dump_got(lib_t *l, const char *when) {
    const char *gv = getenv("DUMP_GOT_AT");
    if (!gv || !l) return;
    unsigned long want = strtoul(gv, NULL, 0);
    if (want < l->minv || want >= l->minv + l->mapsize) return;
    uint64_t v = *(uint64_t *)((char *)l->base + (want - l->minv));
    printf("  [got:%s] %s *(0x%lx) = 0x%lx\n", when,
           strrchr(l->name, '/') ? strrchr(l->name, '/') + 1 : l->name, want, (unsigned long)v);
}

static void *load_so(const char *path) {
    lib_t *l = load_elf(path);
    if (!l) { printf("  [dlopen] load_elf failed for %s\n", path); return NULL; }
    dump_got(l, "loaded");
    register_unwind(l);
    if (g_skip_cxx_init && (strstr(path, "libstdc++") || strstr(path, "libgcc_s"))) {
        printf("  [skip] init_array of %s\n", path);
        return l;
    }
    run_inits(l);
    dump_got(l, "inited");
    return l;
}

/* 显卡 UMD 依赖的 C/C++ 运行时，必须先于 amdxc64.so 加载 */
static void preload_runtime(void) {
    const char *deps[] = {
        "/data/local/tmp/wsl_drivers/libz.so.1",
        "/data/local/tmp/wsl_drivers/libgcc_s.so.1",
        "/data/local/tmp/wsl_drivers/libstdc++.so.6",
    };
    for (size_t i = 0; i < sizeof(deps) / sizeof(deps[0]); i++) {
        if (access(deps[i], R_OK) != 0) { printf("preload skip %s (missing)\n", deps[i]); continue; }
        printf("preload %s\n", deps[i]);
        load_so(deps[i]);
    }
}

/* ---------------- DXCore probe (bionic side) ---------------- */
static const GUID IID_IDXCoreAdapterFactory = {0x78ee5945,0xc36e,0x4b13,{0xa6,0x69,0x00,0x5d,0xd1,0x1c,0x0f,0x06}};
static const GUID IID_IDXCoreAdapterList    = {0x526c7776,0x40e9,0x459b,{0xb7,0x11,0xf3,0x2a,0xd7,0x6d,0xfc,0x28}};
static const GUID IID_IDXCoreAdapter        = {0xf0db4c7f,0xfe5a,0x42a2,{0xbd,0x62,0xf2,0xa6,0xcf,0x6f,0xc8,0x3e}};
static const GUID ATTR_D3D12_GRAPHICS       = {0x0c9ece4d,0x2f6e,0x4f01,{0x8c,0x96,0xe8,0x9e,0x33,0x1b,0x47,0xb1}};
static const GUID CLSID_D3D12DeviceFactory  = {0x114863bf,0xc386,0x4aee,{0xb3,0x9d,0x8f,0x0b,0xbb,0x06,0x29,0x55}};
static const GUID IID_ID3D12DeviceFactory   = {0x61f307d3,0xd34e,0x4e7c,{0x83,0x74,0x3b,0xa4,0xde,0x23,0xcc,0xcb}};
static const GUID IID_ID3D12Device3         = {0x81dadc15,0x2bad,0x4392,{0x93,0xc5,0x10,0x13,0x45,0xc4,0xaa,0x98}};

static void **vtof(void *o) { return *(void***)o; }

static int run_dxcore_probe(lib_t *dx, lib_t *ld, void **adapter_out, void **dev_out) {
    void *best_dev = NULL;
    preload_runtime();
    long (*mk)(const GUID*, void**) = (long(*)(const GUID*,void**))find_symbol_in(dx, "DXCoreCreateAdapterFactory");
    if (!mk) { printf("no DXCoreCreateAdapterFactory\n"); return -1; }
    printf("DXCoreCreateAdapterFactory @ %p\n", (void*)mk);
    void *f = NULL;
    long hr = mk(&IID_IDXCoreAdapterFactory, &f);
    printf("  CreateAdapterFactory hr=0x%08lx f=%p\n", (unsigned long)hr, f);
    if (hr || !f) return -1;

    void *list = NULL;
    hr = ((long(*)(void*,uint32_t,const GUID*,const GUID*,void**))vtof(f)[3])(f, 1, &ATTR_D3D12_GRAPHICS, &IID_IDXCoreAdapterList, &list);
    printf("  CreateAdapterList hr=0x%08lx list=%p\n", (unsigned long)hr, list);
    if (hr || !list) { ((unsigned long(*)(void*))vtof(f)[2])(f); return -1; }

    uint32_t n = ((uint32_t(*)(void*))vtof(list)[4])(list);
    printf("  adapter count = %u\n", n);
    void *ad = NULL;
    for (uint32_t i = 0; i < n; i++) {
        void *a = NULL;
        hr = ((long(*)(void*,uint32_t,const GUID*,void**))vtof(list)[3])(list, i, &IID_IDXCoreAdapter, &a);
        printf("  adapter[%u] hr=0x%08lx a=%p valid=%d\n", i, (unsigned long)hr, a, a ? ((int(*)(void*))vtof(a)[3])(a) : -1);
        if (!a) continue;
        if (env_on("DUMP_ADAPTER")) {
            void **avt = *(void***)a;
            for (int k = 0; k < 20; k++) {
                unsigned long o = 0; int li = lib_of((unsigned long)avt[k], &o);
                printf("    advt[%d] = %s+0x%lx\n", k, li >= 0 ? libshort(g_libs[li].name) : "?", o);
            }
        }
        size_t sz = 0;
        hr = ((long(*)(void*,int,size_t*))vtof(a)[7])(a, 0, &sz);   /* InstanceLuid */
        printf("    LUID propsize hr=0x%08lx size=%zu\n", (unsigned long)hr, sz);
        if (hr == 0 && sz == 8) {
            uint64_t luid = 0;
            hr = ((long(*)(void*,int,size_t,void*))vtof(a)[6])(a, 0, 8, &luid);
            printf("    LUID = %08x:%08x (hr=0x%08lx)\n", (unsigned)(luid>>32), (unsigned)luid, (unsigned long)hr);
        }
        sz = 0; hr = ((long(*)(void*,int,size_t*))vtof(a)[7])(a, 2, &sz);  /* DriverDescription */
        if (hr == 0 && sz < 512) {
            char desc[512]; memset(desc, 0, sizeof desc);
            ((long(*)(void*,int,size_t,void*))vtof(a)[6])(a, 2, sz, desc);
            printf("    desc = '%s'\n", desc);
        }
        if (!ad) ad = a;
    }

    if (ld) {
        long (*getif)(const GUID*, const GUID*, void**) = (long(*)(const GUID*,const GUID*,void**))find_symbol_in(ld, "D3D12GetInterface");
        if (getif) {
            void *df = NULL;
            hr = getif(&CLSID_D3D12DeviceFactory, &IID_ID3D12DeviceFactory, &df);
            printf("  D3D12GetInterface(DeviceFactory) hr=0x%08lx df=%p\n", (unsigned long)hr, df);
            if (!hr && df) {
                /* Mesa 的 create_device 在非 _WIN32 编译下会无条件调用
                 * factory->EnableExperimentalFeatures(1, &D3D12ExperimentalShaderModels)，
                 * 失败就直接返回 nullptr。先单独验一下这个调用的 hr。
                 * ID3D12DeviceFactory vtable: 3 InitializeFromGlobalState / 4 ApplyToGlobalState /
                 * 5 SetFlags / 6 GetFlags / 7 GetConfigurationInterface / 8 EnableExperimentalFeatures
                 * / 9 CreateDevice。 */
                {
                    static const GUID G_EXP = {0x76f5573e,0xf13a,0x40f5,{0xb2,0x97,0x81,0xce,0x9e,0x18,0x93,0x3f}};
                    long h2 = ((long(*)(void*,unsigned,const GUID*,void*,unsigned*))vtof(df)[8])(df, 1, &G_EXP, NULL, NULL);
                    printf("    [mesa-probe] factory->EnableExperimentalFeatures(ShaderModels) hr=0x%08lx\n", (unsigned long)h2);
                }
                /* 这个 WSL 版本的 ID3D12DeviceFactory vtable 布局和 Windows SDK 头文件不一致：
                 * CreateDevice 在 slot[9]，且 riid 必须是 ID3D12Device3（传基础版 ID3D12Device
                 * 会得到 E_NOINTERFACE）。宿主上用 Mesa 抓调用确认过。 */
                for (int fl_i = 0; fl_i < 2; fl_i++) {
                    int fl = fl_i == 0 ? 0x100 : 0xb000;
                    void *dev = NULL;
                    hr = ((long(*)(void*,void*,int,const GUID*,void**))vtof(df)[9])(df, ad, fl, &IID_ID3D12Device3, &dev);
                    printf("    slot[9] CreateDevice(ad, FL=0x%x, IID_ID3D12Device3) hr=0x%08lx dev=%p\n",
                           fl, (unsigned long)hr, dev);
                    if (!hr && dev) {
                        printf("    *** DEVICE CREATED IN BIONIC ***\n");
                        best_dev = dev;
                        break;
                    }
                }
                void *dev0 = NULL;
                hr = ((long(*)(void*,void*,int,const GUID*,void**))vtof(df)[9])(df, ad, 0x100, &IID_ID3D12Device, &dev0);
                printf("    (对比) slot[9] 传基础 IID_ID3D12Device hr=0x%08lx dev=%p\n", (unsigned long)hr, dev0);
            }
        } else printf("  no D3D12GetInterface\n");
    }
    if (adapter_out) *adapter_out = ad;
    if (dev_out) *dev_out = best_dev;
    return 0;
}

/* ---------------- D3D12 真实工作流 smoke test ----------------
 * 目标：证明 bionic 里不只能"建出设备"，还能走完 Mesa d3d12 驱动真正需要的那几跳：
 * 建队列 → 建分配器 → 建命令列表 → Close → ExecuteCommandLists → Signal → 等栅栏
 * → UPLOAD 堆资源 + Map/Unmap。这些全部经过 UMD(amdxc64) → dxgkrnl(/dev/dxg)。 */
static const GUID IID_D3D12_CommandQueue      = {0x0ec870a6,0x5d7e,0x4c22,{0x8c,0xfc,0x5b,0xaa,0xe0,0x76,0x16,0xed}};
static const GUID IID_D3D12_CommandAllocator  = {0x6102dee4,0xaf59,0x4b09,{0xb9,0x99,0xb4,0x4d,0x73,0xf0,0x9b,0x24}};
static const GUID IID_D3D12_GraphicsCmdList   = {0x5b160d0f,0xac1b,0x4185,{0x8b,0xa8,0xb3,0xae,0x42,0xa5,0x4d,0x2c}};
static const GUID IID_D3D12_GraphicsCmdList1  = {0x553103fb,0x1fe7,0x4557,{0xbb,0x38,0x94,0x6d,0x7d,0x0e,0x7c,0xa7}};
static const GUID IID_D3D12_GraphicsCmdList2  = {0x38c3e585,0xff17,0x412c,{0x91,0x50,0x4f,0xc6,0xa9,0xb3,0xa6,0xff}};
static const GUID IID_D3D12_GraphicsCmdList3  = {0x6fda83a7,0xb84c,0x4e38,{0x9a,0xc8,0xc7,0xbd,0x22,0x01,0x6b,0x3d}};
static const GUID IID_D3D12_GraphicsCmdList4  = {0x8754318e,0xd3a9,0x4541,{0x98,0xcf,0x64,0x5b,0x50,0xdc,0x44,0xc7}};
static const GUID IID_D3D12_GraphicsCmdList5  = {0x55050859,0x4024,0x465f,{0x87,0x55,0x6e,0x89,0x8c,0x93,0xa4,0xd9}};
static const GUID IID_D3D12_GraphicsCmdList6  = {0xc3827890,0xe548,0x4cfa,{0x96,0xcf,0x56,0x89,0xa9,0x37,0x0f,0x80}};
static const GUID IID_D3D12_GraphicsCmdList7  = {0xdd1719cf,0xbff7,0x481f,{0xad,0x48,0x8f,0x87,0x66,0xed,0x3d,0x1f}};
static const GUID IID_D3D12_Fence             = {0x0a753dcf,0xc4d8,0x4b91,{0xad,0xf6,0xbe,0x5a,0x60,0xd9,0x5a,0x76}};
static const GUID IID_D3D12_Resource          = {0x696442be,0xa72e,0x4059,{0xbc,0x79,0x5b,0x5c,0x98,0x04,0x0f,0xad}};
/* 8efb471d-616c-4f49-90f7-127bb763fa51：已在 libd3d12.so(0x2a04c) 与 libd3d12core.so(0x1492b0) 二进制中核对该 16 字节常量 */
static const GUID IID_ID3D12_DescriptorHeap = {0x8efb471d,0x616c,0x4f49,{0x90,0xf7,0x12,0x7b,0xb7,0x63,0xfa,0x51}};

typedef struct { int Type; int Priority; int Flags; unsigned NodeMask; } D3D12_CMD_QUEUE_DESC;
typedef struct { int Type; int CPUPageProperty; int MemoryPoolPreference; unsigned CreationNodeMask; unsigned VisibleNodeMask; } D3D12_HEAP_PROPS;
typedef struct { unsigned Count; unsigned Quality; } DXGI_SAMPLE_DESC_T;
typedef struct {
    int Dimension; unsigned _pad;
    unsigned long long Alignment;
    unsigned long long Width;
    unsigned Height;
    unsigned short DepthOrArraySize;
    unsigned short MipLevels;
    int Format;
    DXGI_SAMPLE_DESC_T SampleDesc;
    int Layout;
    unsigned Flags;
} D3D12_RESOURCE_DESC_T;

static long rel_(void *o) { return o ? ((unsigned long(*)(void*))vtof(o)[2])(o) : 0; }
static void hr_msg(const char *what, long hr) { printf("    %-42s hr=0x%08lx\n", what, (unsigned long)hr); }

static int run_d3d12_smoke(lib_t *ld, void *adapter, void *dev_pre) {
    long hr;
    printf("== D3D12 smoke (public API) ==\n");

    /* 1. 公开入口 D3D12CreateDevice —— Mesa 的 d3d12 驱动走的就是它。
     *    传 NULL adapter 时 d3d12core 问 DXGI 要默认适配器，WSL 里没有 → 0x887a0004；
     *    Mesa 在 WSL/Doze 上走 d3d12_dxcore 路径，传的是 IDXCoreAdapter。 */
    long (*cdev)(void*, int, const GUID*, void**) = (long(*)(void*,int,const GUID*,void**))find_symbol_in(ld, "D3D12CreateDevice");
    if (!cdev) { printf("  no D3D12CreateDevice\n"); return -1; }
    void *dnull = NULL, *dadapt = NULL;
    hr = cdev(NULL, 0xb000, &IID_ID3D12Device3, &dnull);
    printf("  D3D12CreateDevice(NULL, FL11_0)        hr=0x%08lx dev=%p\n", (unsigned long)hr, dnull);
    if (adapter) {
        hr = cdev(adapter, 0xb000, &IID_ID3D12Device3, &dadapt);
        printf("  D3D12CreateDevice(IDXCoreAdapter, 11_0) hr=0x%08lx dev=%p\n", (unsigned long)hr, dadapt);
    }
    void *dev = dadapt ? dadapt : (dev_pre ? dev_pre : dnull);
    if (!dev) { printf("  *** no device, stop ***\n"); return -1; }
    printf("  using device %p%s\n", dev, dadapt ? " (public API + dxcore adapter)" : " (dxcore factory path)");

    void **v = vtof(dev);
    unsigned nodes = ((unsigned(*)(void*))v[7])(dev);   /* ID3D12Device: 0-2 IUnknown, 3-6 ID3D12Object, 7 GetNodeCount */
    printf("  GetNodeCount = %u\n", nodes);

    /* 2. 命令队列 */
    D3D12_CMD_QUEUE_DESC qd; memset(&qd, 0, sizeof qd);
    qd.Type = 0; qd.Priority = 0; qd.Flags = 0; qd.NodeMask = 0;
    void *queue = NULL;
    hr = ((long(*)(void*, const D3D12_CMD_QUEUE_DESC*, const GUID*, void**))v[8])(dev, &qd, &IID_D3D12_CommandQueue, &queue);
    hr_msg("CreateCommandQueue(DIRECT)", hr);
    if (!queue) return -1;

    /* 3. 分配器 */
    void *alloc = NULL;
    hr = ((long(*)(void*, int, const GUID*, void**))v[9])(dev, 0, &IID_D3D12_CommandAllocator, &alloc);
    hr_msg("CreateCommandAllocator(DIRECT)", hr);
    if (!alloc) return -1;

    /* 4. 命令列表 */
    for (int i = 0; i < 64; i++) {
        unsigned long fp = (unsigned long)v[i];
        unsigned long o2;
        int k2 = lib_of(fp, &o2);
        if (k2 >= 0 && fp) printf("    device v[%d] = %s+0x%lx\n", i, libshort(g_libs[k2].name), o2);
    }
    struct { const GUID *g; const char *n; } clis[] = {
        {&IID_D3D12_GraphicsCmdList,  "GCL"},
        {&IID_D3D12_GraphicsCmdList1, "GCL1"},
        {&IID_D3D12_GraphicsCmdList2, "GCL2"},
        {&IID_D3D12_GraphicsCmdList3, "GCL3"},
        {&IID_D3D12_GraphicsCmdList4, "GCL4"},
        {&IID_D3D12_GraphicsCmdList5, "GCL5"},
        {&IID_D3D12_GraphicsCmdList6, "GCL6"},
        {&IID_D3D12_GraphicsCmdList7, "GCL7"},
    };
    void *list = NULL;
    for (unsigned k = 0; k < sizeof(clis)/sizeof(clis[0]) && !list; k++) {
        hr = ((long(*)(void*, unsigned, int, void*, void*, const GUID*, void**))v[12])(dev, 0, 0, alloc, NULL, clis[k].g, &list);
        printf("    CreateCommandList(%s) hr=0x%08lx list=%p\n", clis[k].n, (unsigned long)hr, list);
    }
    if (!list) return -1;

    hr = ((long(*)(void*))vtof(list)[9])(list);   /* ID3D12GraphicsCommandList::Close */
    hr_msg("CommandList::Close", hr);

    /* 5. 提交 + 栅栏等待：真正经过 UMD → dxgkrnl 的一跳 */
    void *cl = list;
    hr = ((long(*)(void*, unsigned, void**))vtof(queue)[10])(queue, 1, &cl);  /* ExecuteCommandLists */
    hr_msg("CommandQueue::ExecuteCommandLists(1)", hr);

    void *fence = NULL;
    hr = ((long(*)(void*, unsigned long long, int, const GUID*, void**))v[36])(dev, 0, 0, &IID_D3D12_Fence, &fence);
    hr_msg("CreateFence", hr);

    /* Mesa 的 d3d12_init_screen 末尾用的正是这些调用，逐个复现看谁返回 0x887a0020 */
    for (int fl = 1; fl <= 2; fl++) {
        void *f2 = NULL;
        long h2 = ((long(*)(void*, unsigned long long, int, const GUID*, void**))v[36])(dev, 0, fl, &IID_D3D12_Fence, &f2);
        printf("    [mesa-probe] CreateFence(flags=%d) hr=0x%08lx f=%p\n", fl, (unsigned long)h2, f2);
    }
    {
        struct { int type, prio, flags; unsigned mask; } qd;
        for (int fl = 0; fl <= 2; fl++) {
            qd.type = 0; qd.prio = 0; qd.flags = fl; qd.mask = 0;
            void *q2 = NULL;
            long h2 = ((long(*)(void*, void*, const GUID*, void**))v[8])(dev, &qd, &IID_D3D12_CommandQueue, &q2);
            printf("    [mesa-probe] CreateCommandQueue(flags=%d) hr=0x%08lx q=%p\n", fl, (unsigned long)h2, q2);
        }
    }
    if (!fence) return -1;

    hr = ((long(*)(void*, void*, unsigned long long))vtof(queue)[14])(queue, fence, 1ULL);  /* Signal */
    hr_msg("CommandQueue::Signal(1)", hr);

    unsigned long long done = 0;
    int spins = 0;
    for (; spins < 20000; spins++) {
        done = ((unsigned long long(*)(void*))vtof(fence)[8])(fence);  /* Fence: 0-2 IUnknown, 3-6 Object, 7 GetDevice, 8 GetCompletedValue */
        if (done >= 1) break;
        usleep(500);
    }
    printf("  fence completed value = %llu after %d spins\n", done, spins);
    if (done < 1) {
        long dr = ((long(*)(void*))v[37])(dev);   /* GetDeviceRemovedReason */
        printf("  !! fence never signaled; GetDeviceRemovedReason=0x%08lx\n", (unsigned long)dr);
    } else {
        printf("  *** GPU SUBMIT + FENCE WAIT OK IN BIONIC ***\n");
    }

    /* 6. 真·GPU 回读：UPLOAD buffer → CommandList::CopyBufferRegion → READBACK buffer
     *    Layout 必须是 ROW_MAJOR(1)，否则 core 报
     *    "D3D12_RESOURCE_DESC::Layout must be D3D12_TEXTURE_LAYOUT_ROW_MAJOR when
     *     D3D12_RESOURCE_DESC::Dimension is D3D12_RESOURCE_DIMENSION_BUFFER"。 */
    void *up = NULL, *rdb = NULL;
    D3D12_HEAP_PROPS hp; memset(&hp, 0, sizeof hp);
    hp.CPUPageProperty = 0; hp.MemoryPoolPreference = 0;
    hp.CreationNodeMask = 1; hp.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC_T rd; memset(&rd, 0, sizeof rd);
    rd.Dimension = 1; rd.Alignment = 0; rd.Width = 4096; rd.Height = 1;
    rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = 0;
    rd.SampleDesc.Count = 1; rd.SampleDesc.Quality = 0;
    rd.Layout = 1; rd.Flags = 0;
    printf("    sizeof(desc)=%zu sizeof(heap_props)=%zu\n", sizeof(D3D12_RESOURCE_DESC_T), sizeof(D3D12_HEAP_PROPS));

    hp.Type = 2;   /* D3D12_HEAP_TYPE_UPLOAD */
    hr = ((long(*)(void*, const D3D12_HEAP_PROPS*, unsigned, const D3D12_RESOURCE_DESC_T*, int,
                   const void*, const GUID*, void**))v[27])(dev, &hp, 0, &rd, 0xac3, NULL, &IID_D3D12_Resource, &up);
    printf("  CreateCommittedResource(UPLOAD 4096, ROW_MAJOR)   hr=0x%08lx up=%p\n", (unsigned long)hr, up);
    hp.Type = 3;   /* D3D12_HEAP_TYPE_READBACK */
    hr = ((long(*)(void*, const D3D12_HEAP_PROPS*, unsigned, const D3D12_RESOURCE_DESC_T*, int,
                   const void*, const GUID*, void**))v[27])(dev, &hp, 0, &rd, 0x400, NULL, &IID_D3D12_Resource, &rdb);
    printf("  CreateCommittedResource(READBACK 4096)            hr=0x%08lx rdb=%p\n", (unsigned long)hr, rdb);
    if (!up || !rdb) { printf("  !! buffer alloc failed, stop\n"); }

    unsigned char *p = NULL;
    if (up) {
        hr = ((long(*)(void*, unsigned, const void*, void**))vtof(up)[8])(up, 0, NULL, (void**)&p);
        printf("  Resource::Map(upload,0) hr=0x%08lx ptr=%p\n", (unsigned long)hr, p);
        if (!hr && p) {
            for (int i = 0; i < 4096; i++) p[i] = (unsigned char)(i * 7 + 3);
            ((void(*)(void*, unsigned, const void*))vtof(up)[9])(up, 0, NULL);   /* Unmap */
            printf("    filled 4096 bytes pattern, unmapped\n");
        } else p = NULL;
    }
    void *list2 = NULL;
    if (up && rdb) {
        hr = ((long(*)(void*, unsigned, int, void*, void*, const GUID*, void**))v[12])
                (dev, 0, 0, alloc, NULL, &IID_D3D12_GraphicsCmdList1, &list2);
        printf("  CreateCommandList#2(GCL1) hr=0x%08lx list=%p\n", (unsigned long)hr, list2);
    }
    if (list2) {
        hr = ((long(*)(void*, void*, unsigned long long, void*, unsigned long long, unsigned long long))
              vtof(list2)[15])(list2, rdb, 0, up, 0, 4096);          /* CopyBufferRegion */
        printf("  CommandList::CopyBufferRegion(4096) hr=0x%08lx\n", (unsigned long)hr);
        hr = ((long(*)(void*))vtof(list2)[9])(list2);                 /* Close */
        printf("  Close#2 hr=0x%08lx\n", (unsigned long)hr);
        void *cl2 = list2;
        ((void(*)(void*, unsigned, void**))vtof(queue)[10])(queue, 1, &cl2);  /* ExecuteCommandLists */
        hr = ((long(*)(void*, void*, unsigned long long))vtof(queue)[14])(queue, fence, 2ULL);  /* Signal(2) */
        printf("  Execute + Signal(2) hr=0x%08lx\n", (unsigned long)hr);
        unsigned long long done = 0;
        int spins = 0;
        for (; spins < 20000; spins++) {
            done = ((unsigned long long(*)(void*))vtof(fence)[8])(fence);
            if (done >= 2) break;
            usleep(500);
        }
        printf("  fence completed = %llu after %d spins\n", done, spins);
        unsigned char *rp = NULL;
        hr = ((long(*)(void*, unsigned, const void*, void**))vtof(rdb)[8])(rdb, 0, NULL, (void**)&rp);
        printf("  Resource::Map(readback,0) hr=0x%08lx ptr=%p\n", (unsigned long)hr, rp);
        if (!hr && rp) {
            int bad = 0;
            for (int i = 0; i < 4096; i++)
                if (rp[i] != (unsigned char)(i * 7 + 3)) {
                    if (bad < 4) printf("    mismatch@%d got=%02x want=%02x\n", i, rp[i], (unsigned char)(i * 7 + 3));
                    bad++;
                }
            ((void(*)(void*, unsigned, const void*))vtof(rdb)[9])(rdb, 0, NULL);
            if (done >= 2 && bad == 0) printf("  *** GPU COPY ROUNDTRIP OK IN BIONIC ***\n");
            else printf("  !! GPU COPY ROUNDTRIP FAIL bad=%d done=%llu\n", bad, done);
        }
    }

    /* 6b. ClearRenderTargetView → CopyTextureRegion → READBACK
     *     验证渲染命令路径（DescriptorHeap / RTV / BARRIER / CLEAR），全程不需要 shader。 */
    {
        long h;
        D3D12_HEAP_PROPS hp2; memset(&hp2, 0, sizeof hp2);
        hp2.Type = 1; hp2.CreationNodeMask = 1; hp2.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC_T td; memset(&td, 0, sizeof td);
        td.Dimension = 3;              /* TEXTURE2D */
        td.Width = 4; td.Height = 4; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = 28;                /* DXGI_FORMAT_R8G8B8A8_UNORM */
        td.SampleDesc.Count = 1; td.Layout = 0;
        td.Flags = 1;                  /* ALLOW_RENDER_TARGET */
        void *tex = NULL;
        h = ((long(*)(void*, const D3D12_HEAP_PROPS*, unsigned, const D3D12_RESOURCE_DESC_T*, int,
                     const void*, const GUID*, void**))v[27])(dev, &hp2, 0, &td, 4 /*RENDER_TARGET*/,
                                                             NULL, &IID_D3D12_Resource, &tex);
        printf("  CreateCommittedResource(TEX 4x4 RGBA8, RTV)   hr=0x%08lx tex=%p\n", (unsigned long)h, tex);

        struct { int Type; unsigned NumDescriptors; int Flags; unsigned NodeMask; } hd2;
        memset(&hd2, 0, sizeof hd2); hd2.Type = 2; hd2.NumDescriptors = 1;  /* RTV heap */
        /* 8efb471d-616c-4f49-90f7-127bb763fa51 已提到文件作用域（compute 段也要用） */
        static const GUID IID_ID3D12Object2 = {0x905db94b,0xa00c,0x4140,{0x9d,0xf5,0x2b,0x64,0xca,0x9e,0xa3,0x57}};
        void *dh = NULL;
        h = ((long(*)(void*, const void*, const GUID*, void**))v[14])(dev, &hd2, &IID_ID3D12_DescriptorHeap, &dh);
        printf("  CreateDescriptorHeap(RTV,1)                   hr=0x%08lx dh=%p\n", (unsigned long)h, dh);
        if (!dh) {
            /* IUnknown 对任何 COM 对象都必然成功，用它拿主 vtable，避免乱猜 IID */
            static const GUID IID_IUnknown3 = {0x00000000,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
            h = ((long(*)(void*, const void*, const GUID*, void**))v[14])(dev, &hd2, &IID_IUnknown3, &dh);
            printf("  CreateDescriptorHeap(retry IID_IUnknown)      hr=0x%08lx dh=%p\n", (unsigned long)h, dh);
        }
        if (dh) {
            void **hv = vtof(dh);
            for (int i = 0; i <= 12; i++) {
                unsigned long fp = (unsigned long)hv[i];
                for (int k = 0; k < g_nlibs; k++) {
                    unsigned long b = (unsigned long)g_libs[k].base;
                    if (fp >= b && fp < b + g_libs[k].mapsize)
                        printf("    heap v[%d] = %s+0x%lx\n", i, g_libs[k].name, fp - b);
                }
            }
            printf("    dev  v[3..6] = %p %p %p %p\n", vtof(dev)[3], vtof(dev)[4], vtof(dev)[5], vtof(dev)[6]);
            printf("    heap v[3..6] = %p %p %p %p\n", hv[3], hv[4], hv[5], hv[6]);
            struct { int Type; unsigned NumDescriptors; int Flags; unsigned NodeMask; } gd;
            memset(&gd, 0, sizeof gd);
            ((void(*)(void*, void*))hv[10])(dh, &gd);
            printf("    heap v[10] desc: Type=%d Num=%u Flags=0x%x NodeMask=0x%x\n", gd.Type, gd.NumDescriptors, gd.Flags, gd.NodeMask);
        }
        if (tex && dh) {
            unsigned long long h8 = ((unsigned long long(*)(void*))vtof(dh)[8])(dh);
            printf("    heap v[8] (cpu?) = 0x%llx\n", h8);
            unsigned long long h9 = ((unsigned long long(*)(void*))vtof(dh)[9])(dh);
            printf("    heap v[9] (gpu?) = 0x%llx\n", h9);
            /* 实测反了：slot[8] 返回 0x100000002（GPU handle 形态），slot[9] 返回真指针 → CPU handle 用 slot[9] */
            unsigned long long cpu = h9;
            struct { int Format; int ViewDimension; unsigned MipSlice, PlaneSlice, FirstArraySlice, ArraySize; } rtv;
            memset(&rtv, 0, sizeof rtv);
            rtv.Format = 28; rtv.ViewDimension = 4; rtv.ArraySize = 1;   /* TEX2D */
            ((void(*)(void*, void*, const void*, unsigned long long))v[20])(dev, tex, &rtv, cpu);
            printf("    CreateRenderTargetView cpu=0x%llx\n", cpu);

            /* readback buffer：4 行 × 256 对齐 pitch */
            void *rrb = NULL;
            D3D12_RESOURCE_DESC_T bd; memset(&bd, 0, sizeof bd);
            bd.Dimension = 1; bd.Width = 1024; bd.Height = 1; bd.DepthOrArraySize = 1;
            bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = 1;   /* ROW_MAJOR */
            D3D12_HEAP_PROPS hpr; memset(&hpr, 0, sizeof hpr);
            hpr.Type = 3; hpr.CreationNodeMask = 1; hpr.VisibleNodeMask = 1;
            h = ((long(*)(void*, const D3D12_HEAP_PROPS*, unsigned, const D3D12_RESOURCE_DESC_T*, int,
                         const void*, const GUID*, void**))v[27])(dev, &hpr, 0, &bd, 0x400, NULL,
                                                                &IID_D3D12_Resource, &rrb);
            printf("  CreateCommittedResource(READBACK 1024)        hr=0x%08lx rrb=%p\n", (unsigned long)h, rrb);

            void *list3 = NULL;
            h = ((long(*)(void*, unsigned, int, void*, void*, const GUID*, void**))v[12])
                    (dev, 0, 0, alloc, NULL, &IID_D3D12_GraphicsCmdList1, &list3);
            printf("  CreateCommandList#3(GCL1) hr=0x%08lx list=%p\n", (unsigned long)h, list3);
            if (list3 && rrb) {
                void **lv = vtof(list3);
                struct { void *pResource; unsigned Subresource; int StateBefore; int StateAfter; } tr;
                struct { int Type; int Flags; unsigned char u[24]; } bar;
                memset(&bar, 0, sizeof bar); bar.Type = 0;   /* TRANSITION */
                /* 顺序：Clear 必须在 RENDER_TARGET(4) 状态下做，之后才能切 COPY_SOURCE(0x800) 去拷贝 */
                float color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
                ((void(*)(void*, unsigned long long, const float*, unsigned, const void*))lv[48])
                        (list3, cpu, color, 0, NULL);
                tr.pResource = tex; tr.Subresource = 0xFFFFFFFF; tr.StateBefore = 4; tr.StateAfter = 0x800;
                memcpy(bar.u, &tr, sizeof tr);
                ((void(*)(void*, unsigned, const void*))lv[26])(list3, 1, &bar);   /* ResourceBarrier */

                struct { unsigned Format, Width, Height, Depth, RowPitch; } fp;
                fp.Format = 28; fp.Width = 4; fp.Height = 4; fp.Depth = 1; fp.RowPitch = 256;
                struct { void *pResource; int Type; int pad; union { struct { unsigned long long Offset; struct { unsigned F, W, H, D, P; } fp2; } pf; unsigned sub; } u; } dst, src;
                memset(&dst, 0, sizeof dst); memset(&src, 0, sizeof src);
                dst.pResource = rrb; dst.Type = 1;         /* PLACED_FOOTPRINT */
                dst.u.pf.Offset = 0;
                dst.u.pf.fp2.F = fp.Format; dst.u.pf.fp2.W = fp.Width; dst.u.pf.fp2.H = fp.Height;
                dst.u.pf.fp2.D = fp.Depth; dst.u.pf.fp2.P = fp.RowPitch;
                src.pResource = tex; src.Type = 0; src.u.sub = 0;   /* SUBRESOURCE */
                ((long(*)(void*, const void*, unsigned, unsigned, unsigned, const void*, const void*))lv[16])
                        (list3, &dst, 0, 0, 0, &src, NULL);            /* CopyTextureRegion */
                h = ((long(*)(void*))lv[9])(list3);
                printf("    Clear + Copy recorded, Close hr=0x%08lx\n", (unsigned long)h);
                void *cl3 = list3;
                ((void(*)(void*, unsigned, void**))vtof(queue)[10])(queue, 1, &cl3);
                ((long(*)(void*, void*, unsigned long long))vtof(queue)[14])(queue, fence, 3ULL);
                unsigned long long dv = 0;
                for (int i = 0; i < 20000; i++) {
                    dv = ((unsigned long long(*)(void*))vtof(fence)[8])(fence);
                    if (dv >= 3) break;
                    usleep(500);
                }
                printf("  clear/copy fence = %llu\n", dv);
                unsigned char *rp = NULL;
                h = ((long(*)(void*, unsigned, const void*, void**))vtof(rrb)[8])(rrb, 0, NULL, (void**)&rp);
                printf("  Map(readback) hr=0x%08lx ptr=%p\n", (unsigned long)h, rp);
                if (!h && rp) {
                    printf("  pixel[0]=%02x %02x %02x %02x  pixel[1][1]=%02x %02x %02x %02x\n",
                           rp[0], rp[1], rp[2], rp[3], rp[256+4], rp[256+5], rp[256+6], rp[256+7]);
                    int ok = abs((int)rp[0] - 64) <= 1 && abs((int)rp[1] - 128) <= 1 &&
                             abs((int)rp[2] - 191) <= 1 && rp[3] >= 254;
                    printf("  *** CLEAR RENDER TARGET %s IN BIONIC (want 40 80 bf ff) ***\n", ok ? "OK" : "MISMATCH");
                    ((void(*)(void*, unsigned, const void*))vtof(rrb)[9])(rrb, 0, NULL);
                }
                rel_(list3);
            }
            if (rrb) rel_(rrb);
            rel_(dh);
        }
        if (tex) rel_(tex);
    }

    /* 6c. DXIL compute dispatch：UAV buffer + root signature + PSO + Dispatch + 回读 */
    {
        FILE *cf = fopen("/data/local/tmp/cs.dxil", "rb");
        if (!cf) {
            printf("  [6c] /data/local/tmp/cs.dxil missing, skip compute test\n");
        } else {
            fseek(cf, 0, SEEK_END); long cfl = ftell(cf); fseek(cf, 0, SEEK_SET);
            unsigned char *cbuf = (unsigned char *)malloc(cfl > 0 ? (size_t)cfl : 1);
            if (cbuf && cfl > 0 && fread(cbuf, 1, (size_t)cfl, cf) == (size_t)cfl) {
                printf("  [6c] cs.dxil %ld bytes\n", cfl);
                static const GUID IID_ID3D12_PipelineState = {0x765a30f3,0xf624,0x4c6f,{0xa8,0x28,0xac,0xe9,0x48,0x62,0x24,0x45}};
                static const GUID IID_ID3D12_RootSignature = {0xc54a6b66,0x72df,0x4ee8,{0x8b,0xe5,0xa9,0x46,0xa1,0x42,0x92,0x14}};
                /* D3D12SerializeRootSignature 产物（1 个 UAV descriptor table，visibility ALL，flags 0） */
                static const unsigned char rs_blob[108] = {
                    0x44,0x58,0x42,0x43,0x19,0xbb,0x5b,0x9b,0xc9,0x2a,0x50,0x0c,
                    0x0e,0x2d,0x1c,0xb3,0x70,0x21,0xaa,0x1f,0x01,0x00,0x00,0x00,
                    0x6c,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x24,0x00,0x00,0x00,
                    0x52,0x54,0x53,0x30,0x40,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
                    0x01,0x00,0x00,0x00,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                    0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                    0x00,0x00,0x00,0x00,0x24,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
                    0x2c,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
                    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
                };
                void *rsig = NULL, *pso = NULL, *uav = NULL, *rrb2 = NULL, *dh = NULL, *list4 = NULL, *alloc2 = NULL;
                void **lv = NULL;
                long h;
                h = ((long(*)(void*, unsigned, const void*, size_t, const GUID*, void**))v[16])
                        (dev, 0, rs_blob, sizeof rs_blob, &IID_ID3D12_RootSignature, &rsig);
                printf("  [6c] CreateRootSignature(blob)        hr=0x%08lx rsig=%p\n", (unsigned long)h, rsig);

                struct {
                    void *pRootSignature;
                    const void *pShader;
                    unsigned long long shader_len;
                    unsigned node_mask;
                    unsigned _pad;
                    const void *cached_blob;
                    unsigned long long cached_len;
                    int flags;
                    int _pad2;
                } psd;
                memset(&psd, 0, sizeof psd);
                psd.pRootSignature = rsig;
                psd.pShader = cbuf;
                psd.shader_len = (unsigned long long)cfl;
                h = ((long(*)(void*, const void*, const GUID*, void**))v[11])(dev, &psd, &IID_ID3D12_PipelineState, &pso);
                printf("  [6c] CreateComputePipelineState       hr=0x%08lx pso=%p\n", (unsigned long)h, pso);

                D3D12_HEAP_PROPS hpu; memset(&hpu, 0, sizeof hpu);
                hpu.Type = 1; hpu.CreationNodeMask = 1; hpu.VisibleNodeMask = 1;   /* DEFAULT */
                D3D12_RESOURCE_DESC_T ud; memset(&ud, 0, sizeof ud);
                ud.Dimension = 1; ud.Width = 256; ud.Height = 1; ud.DepthOrArraySize = 1;
                ud.MipLevels = 1; ud.Format = 0; ud.SampleDesc.Count = 1; ud.Layout = 1;
                ud.Flags = 4;   /* ALLOW_UNORDERED_ACCESS */
                h = ((long(*)(void*, const D3D12_HEAP_PROPS*, unsigned, const D3D12_RESOURCE_DESC_T*, int,
                              const void*, const GUID*, void**))v[27])(dev, &hpu, 0, &ud, 0x8, NULL,
                                                                     &IID_D3D12_Resource, &uav);
                printf("  [6c] CreateCommittedResource(UAV DEFAULT) hr=0x%08lx uav=%p\n", (unsigned long)h, uav);

                D3D12_HEAP_PROPS hprr; memset(&hprr, 0, sizeof hprr);
                hprr.Type = 3; hprr.CreationNodeMask = 1; hprr.VisibleNodeMask = 1;   /* READBACK */
                D3D12_RESOURCE_DESC_T rdd = ud; rdd.Flags = 0;
                h = ((long(*)(void*, const D3D12_HEAP_PROPS*, unsigned, const D3D12_RESOURCE_DESC_T*, int,
                              const void*, const GUID*, void**))v[27])(dev, &hprr, 0, &rdd, 0x400, NULL,
                                                                     &IID_D3D12_Resource, &rrb2);
                printf("  [6c] CreateCommittedResource(READBACK) hr=0x%08lx rrb=%p\n", (unsigned long)h, rrb2);

                struct { int Type; unsigned NumDescriptors; int Flags; unsigned NodeMask; } hd3;
                hd3.Type = 0; hd3.NumDescriptors = 1; hd3.Flags = 1; hd3.NodeMask = 1;
                h = ((long(*)(void*, const void*, const GUID*, void**))v[14])(dev, &hd3, &IID_ID3D12_DescriptorHeap, &dh);
                printf("  [6c] CreateDescriptorHeap            hr=0x%08lx dh=%p\n", (unsigned long)h, dh);

                if (rsig && pso && uav && rrb2 && dh) {
                    printf("  [6c] uav VA=0x%llx removed=0x%08lx\n",
                           ((unsigned long long(*)(void*))vtof(uav)[11])(uav),
                           (unsigned long)((long(*)(void*))v[37])(dev));
                    unsigned long long dhv = (unsigned long long)dh;
                    unsigned long long cpu = ((unsigned long long(*)(void*))vtof(dh)[9])(dh);
                    unsigned long long gpu = ((unsigned long long(*)(void*))vtof(dh)[10])(dh);
                    printf("  [6c] heap cpu=0x%llx gpu=0x%llx (dh=0x%llx)\n", cpu, gpu, dhv);
                    /* 注意：D3D12_BUFFER_UAV 的 FirstElement / CounterOffsetInBytes 都是 UINT64，
                     * 整个 desc 是 40 字节（Format+ViewDim 8 + union 32）。写成 28 字节会让 core
                     * 读到越界的 Flags → UAV 创建失败并 RemoveDevice(INVALID_CALL)。 */
                    struct {
                        int Format; int ViewDimension;
                        unsigned long long FirstElement;
                        unsigned NumElements;
                        unsigned StructureByteStride;
                        unsigned long long CounterOffsetInBytes;
                        int Flags;
                        int _pad;
                    } uvd;
                    memset(&uvd, 0, sizeof uvd);
                    uvd.Format = 0; uvd.ViewDimension = 1; uvd.NumElements = 64; uvd.StructureByteStride = 4;
                    ((void(*)(void*, void*, const void*, const void*, unsigned long long))v[19])(dev, uav, NULL, &uvd, cpu);
                    printf("  [6c] CreateUnorderedAccessView done removed=0x%08lx\n",
                           (unsigned long)((long(*)(void*))v[37])(dev));

                    h = ((long(*)(void*, int, const GUID*, void**))v[9])(dev, 0, &IID_D3D12_CommandAllocator, &alloc2);
                    h = ((long(*)(void*, unsigned, int, void*, void*, const GUID*, void**))v[12])
                            (dev, 0, 0, alloc2, NULL, &IID_D3D12_GraphicsCmdList1, &list4);
                    printf("  [6c] CreateCommandList#4 hr=0x%08lx list=%p\n", (unsigned long)h, list4);
                    if (list4) {
                        lv = vtof(list4);
                        ((void(*)(void*, unsigned, void**))lv[28])(list4, 1, &dh);
                        ((void(*)(void*, void*))lv[29])(list4, rsig);
                        ((void(*)(void*, void*))lv[25])(list4, pso);
                        ((void(*)(void*, unsigned, unsigned long long))lv[31])(list4, 0, gpu);
                        ((void(*)(void*, unsigned, unsigned, unsigned))lv[14])(list4, 8, 1, 1);
                        struct { void *pResource; unsigned Subresource; int StateBefore; int StateAfter; } tr4;
                        struct { int Type; int Flags; unsigned char u[24]; } bar4;
                        memset(&bar4, 0, sizeof bar4); bar4.Type = 0;
                        tr4.pResource = uav; tr4.Subresource = 0xFFFFFFFF;
                        tr4.StateBefore = 0x8; tr4.StateAfter = 0x800;
                        memcpy(bar4.u, &tr4, sizeof tr4);
                        ((void(*)(void*, unsigned, const void*))lv[26])(list4, 1, &bar4);
                        ((void(*)(void*, void*, unsigned, void*, unsigned, unsigned))lv[15])(list4, rrb2, 0, uav, 0, 256);
                        h = ((long(*)(void*))lv[9])(list4);
                        printf("  [6c] Dispatch(8,1,1)+copy recorded, Close hr=0x%08lx\n", (unsigned long)h);
                        void *cl4 = list4;
                        ((void(*)(void*, unsigned, void**))vtof(queue)[10])(queue, 1, &cl4);
                        ((long(*)(void*, void*, unsigned long long))vtof(queue)[14])(queue, fence, 4ULL);
                        unsigned long long dv = 0;
                        for (int i = 0; i < 20000; i++) {
                            dv = ((unsigned long long(*)(void*))vtof(fence)[8])(fence);
                            if (dv >= 4) break;
                            usleep(500);
                        }
                        printf("  [6c] fence=%llu\n", dv);
                        unsigned *rp = NULL;
                        h = ((long(*)(void*, unsigned, const void*, void**))vtof(rrb2)[8])(rrb2, 0, NULL, (void**)&rp);
                        printf("  [6c] Map(readback) hr=0x%08lx ptr=%p\n", (unsigned long)h, rp);
                        if (!h && rp) {
                            int bad = 0;
                            for (int i = 0; i < 8; i++) {
                                unsigned want = (unsigned)(i * 3 + 7);
                                if (rp[i] != want) bad++;
                                printf("    out[%d]=%u (want %u)%s\n", i, rp[i], want, rp[i] == want ? "" : "  <-- BAD");
                            }
                            printf("  *** COMPUTE DISPATCH %s IN BIONIC ***\n", bad ? "MISMATCH" : "OK");
                            ((void(*)(void*, unsigned, const void*))vtof(rrb2)[9])(rrb2, 0, NULL);
                        }
                        rel_(list4);
                    }
                    if (alloc2) rel_(alloc2);
                    if (dh) rel_(dh);
                }
                if (pso) rel_(pso);
                if (rsig) rel_(rsig);
                if (uav) rel_(uav);
                if (rrb2) rel_(rrb2);
            }
            if (cbuf) free(cbuf);
            fclose(cf);
        }
    }

    /* 8. Mesa create_device 的等价复现：factory->SetFlags(3) 之后用各种 FEATURE_LEVEL 建设备。
     * Mesa 在 Linux 下先试 D3D_FEATURE_LEVEL_1_0_GENERIC，再退 11_0。 */
    {
        long (*getif2)(const GUID*, const GUID*, void**) = (long(*)(const GUID*,const GUID*,void**))find_symbol_in(ld, "D3D12GetInterface");
        void *df2 = NULL;
        if (getif2 && !getif2(&CLSID_D3D12DeviceFactory, &IID_ID3D12DeviceFactory, &df2) && df2) {
            long hs = ((long(*)(void*,unsigned))vtof(df2)[5])(df2, 3);   /* SetFlags(ALLOW_RETURNING_*) */
            printf("  [flprobe] SetFlags(3) hr=0x%08lx\n", (unsigned long)hs);
            unsigned fls[] = {0x100, 0x200, 0x1000, 0x2000, 0x9100, 0xa000, 0xb000, 0xc000};
            for (unsigned fi = 0; fi < sizeof(fls)/sizeof(fls[0]); fi++) {
                void *d2 = NULL;
                long h2 = ((long(*)(void*,void*,int,const GUID*,void**))vtof(df2)[9])(df2, adapter, (int)fls[fi], &IID_ID3D12Device3, &d2);
                printf("  [flprobe] CreateDevice(FL=0x%x) hr=0x%08lx dev=%p\n", fls[fi], (unsigned long)h2, d2);
                if (d2) ((unsigned long(*)(void*))vtof(d2)[2])(d2);
            }
        } else {
            printf("  [flprobe] D3D12GetInterface failed\n");
        }
    }

    /* 9. 精确复现 Mesa 的 create_device 顺序：EnableExperimentalFeatures → SetFlags(3) → CreateDevice(FL=0x100) */
    {
        static const GUID G_EXP = {0x76f5573e,0xf13a,0x40f5,{0xb2,0x97,0x81,0xce,0x9e,0x18,0x93,0x3f}};
        long (*getif3)(const GUID*, const GUID*, void**) = (long(*)(const GUID*,const GUID*,void**))find_symbol_in(ld, "D3D12GetInterface");
        for (int variant = 0; variant < 4; variant++) {
            void *df3 = NULL;
            if (!getif3 || getif3(&CLSID_D3D12DeviceFactory, &IID_ID3D12DeviceFactory, &df3) || !df3) {
                printf("  [mesaorder] variant %d: D3D12GetInterface failed\n", variant);
                continue;
            }
            long h = 0;
            if (variant & 1) {   /* 先 EnableExperimentalFeatures */
                h = ((long(*)(void*,unsigned,const GUID*,void*,unsigned*))vtof(df3)[8])(df3, 1, &G_EXP, NULL, NULL);
                printf("  [mesaorder] variant %d: EnableExperimentalFeatures hr=0x%08lx\n", variant, (unsigned long)h);
            }
            if (variant & 2) {   /* 再 SetFlags(3) */
                h = ((long(*)(void*,unsigned))vtof(df3)[5])(df3, 3);
                printf("  [mesaorder] variant %d: SetFlags(3) hr=0x%08lx\n", variant, (unsigned long)h);
            }
            void *d3 = NULL;
            h = ((long(*)(void*,void*,int,const GUID*,void**))vtof(df3)[9])(df3, adapter, 0x100, &IID_ID3D12Device3, &d3);
            printf("  [mesaorder] variant %d: CreateDevice(FL=0x100) hr=0x%08lx dev=%p\n", variant, (unsigned long)h, d3);
            if (d3) ((unsigned long(*)(void*))vtof(d3)[2])(d3);
        }
    }

    /* 7. 清理 */
    rel_(fence);
    rel_(list);
    rel_(alloc);
    rel_(queue);
    printf("== D3D12 smoke done ==\n");
    return 0;
}

/* ---------------- Mesa (glibc 版 d3d12 驱动) 探针 ---------------- */
static const char *mesa_dir(void) {
    const char *d = getenv("MESA_DIR");
    return (d && *d) ? d : "/data/local/tmp/mesa";
}

static int run_mesa_probe(void) {
    char dir[256];
    snprintf(dir, sizeof dir, "%s", mesa_dir());
    printf("== mesa probe dir=%s ==\n", dir);

    char p[600];
    snprintf(p, sizeof p, "%s/libgallium-26.0.8.so", dir);
    lib_t *lg = load_so(p);
    if (!lg) { printf("mesa: libgallium load failed\n"); return -1; }
    snprintf(p, sizeof p, "%s/libEGL.so.1.0.0", dir);
    lib_t *egl = load_so(p);
    if (!egl) { printf("mesa: libEGL load failed\n"); return -1; }

    void *(*getPlat)(unsigned, void *, const long *) =
        (void *(*)(unsigned, void *, const long *))find_symbol_in(egl, "eglGetPlatformDisplay");
    int (*init)(void *, int *, int *) = (int (*)(void *, int *, int *))find_symbol_in(egl, "eglInitialize");
    const char *(*qs)(void *, int) = (const char *(*)(void *, int))find_symbol_in(egl, "eglQueryString");
    int (*gerr)(void) = (int (*)(void))find_symbol_in(egl, "eglGetError");
    if (!getPlat || !init || !qs) { printf("mesa: egl entry missing\n"); return -1; }

    g_phase = 500; g_phase_text = "eglGetPlatformDisplay";
    printf("== eglGetPlatformDisplay(SURFACELESS_MESA) ==\n");
    printf("  getPlat=%p bytes:", (void*)getPlat);
    for (int i = 0; i < 16; i++) printf(" %02x", ((unsigned char*)getPlat)[i]);
    printf("\n");
    fflush(stdout);
    void *dpy = getPlat(0x31DD, NULL, NULL);
    printf("  dpy=%p err=0x%x\n", dpy, gerr ? gerr() : 0);
    if (!dpy) return -1;

    int maj = 0, min = 0;
    g_phase_text = "eglInitialize";
    printf("== eglInitialize ==\n");
    int ok = init(dpy, &maj, &min);
    printf("  ok=%d version=%d.%d err=0x%x\n", ok, maj, min, gerr ? gerr() : 0);
    if (!ok) return -1;

    printf("  EGL_VENDOR   = %s\n", qs(dpy, 0x3053));
    printf("  EGL_VERSION  = %s\n", qs(dpy, 0x3054));
    printf("  EGL_CLIENT_APIS = %s\n", qs(dpy, 0x308D));
    const char *ext = qs(dpy, 0x3055);
    if (ext) printf("  EGL_EXTENSIONS[0:200] = %.200s\n", ext);

    int (*bindApi)(unsigned) = (int (*)(unsigned))find_symbol_in(egl, "eglBindAPI");
    int (*chooseConfig)(void *, const void *, void **, int, int *) =
        (int (*)(void *, const void *, void **, int, int *))find_symbol_in(egl, "eglChooseConfig");
    void *(*createCtx)(void *, void *, void *, const void *) =
        (void *(*)(void *, void *, void *, const void *))find_symbol_in(egl, "eglCreateContext");
    void *(*createPbuf)(void *, void *, const void *) =
        (void *(*)(void *, void *, const void *))find_symbol_in(egl, "eglCreatePbufferSurface");
    int (*makeCur)(void *, void *, void *, void *) =
        (int (*)(void *, void *, void *, void *))find_symbol_in(egl, "eglMakeCurrent");
    void *(*getProc)(const char *) = (void *(*)(const char *))find_symbol_in(egl, "eglGetProcAddress");

    g_phase = 510; g_phase_text = "eglBindAPI";
    printf("== eglBindAPI(EGL_OPENGL_ES_API) ==\n");
    if (bindApi) printf("  bindApi=%d err=0x%x\n", bindApi(0x30A0), gerr ? gerr() : 0);

    const int cfgattr[] = { 0x3033, 1 /*PBUFFER*/, 0x3040, 4 /*ES2*/, 0x3038 };
    void *cfg = NULL; int ncfg = 0;
    g_phase_text = "eglChooseConfig";
    int okc = chooseConfig ? chooseConfig(dpy, cfgattr, &cfg, 1, &ncfg) : 0;
    printf("== eglChooseConfig ok=%d n=%d cfg=%p err=0x%x ==\n", okc, ncfg, cfg, gerr ? gerr() : 0);
    if (!okc || !cfg) return -1;

    const int ctxattr[] = { 0x3098, 2, 0x3038 };
    printf("  [tid] before createContext tid=%ld\n", (long)syscall(SYS_gettid));
    g_phase_text = "eglCreateContext";
    void *ctx = createCtx ? createCtx(dpy, cfg, NULL, ctxattr) : NULL;
    printf("== eglCreateContext ctx=%p err=0x%x ==\n", ctx, gerr ? gerr() : 0);
    if (!ctx) return -1;

    const int pattr[] = { 0x3057, 64, 0x3056, 64, 0x3038 };
    g_phase_text = "eglCreatePbufferSurface";
    void *surf = createPbuf ? createPbuf(dpy, cfg, pattr) : NULL;
    printf("== eglCreatePbufferSurface surf=%p err=0x%x ==\n", surf, gerr ? gerr() : 0);

    g_phase = 520; g_phase_text = "eglMakeCurrent";
    int okm = makeCur ? makeCur(dpy, surf, surf, ctx) : 0;
    printf("== eglMakeCurrent ok=%d err=0x%x ==\n", okm, gerr ? gerr() : 0);
    if (okm) {
        g_phase_text = "glGetString";
        const char *(*glGetString)(unsigned) = (const char *(*)(unsigned))getProc("glGetString");
        printf("  glGetString=%p\n", (void*)glGetString);
        if (glGetString) {
            printf("  GL_VENDOR   = %s\n", glGetString(0x1F00));
            printf("  GL_RENDERER = %s\n", glGetString(0x1F01));
            printf("  GL_VERSION  = %s\n", glGetString(0x1F02));
        }
        /* 真的画一帧：清成洋红，再读回一个像素 */
        void (*glViewport)(int,int,int,int) = (void (*)(int,int,int,int))getProc("glViewport");
        void (*glClearColor)(float,float,float,float) = (void (*)(float,float,float,float))getProc("glClearColor");
        void (*glClear)(unsigned) = (void (*)(unsigned))getProc("glClear");
        void (*glFinish)(void) = (void (*)(void))getProc("glFinish");
        void (*glReadPixels)(int,int,int,int,unsigned,unsigned,void*) =
            (void (*)(int,int,int,int,unsigned,unsigned,void*))getProc("glReadPixels");
        printf("  glViewport=%p glClearColor=%p glClear=%p glFinish=%p glReadPixels=%p\n",
               (void*)glViewport, (void*)glClearColor, (void*)glClear, (void*)glFinish, (void*)glReadPixels);
        if (glViewport && glClearColor && glClear && glFinish && glReadPixels) {
            g_phase = 530; g_phase_text = "glClear";
            printf("== glClear(洋红) + glReadPixels ==\n");
            glViewport(0, 0, 64, 64);
            glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
            glClear(0x4000 /* GL_COLOR_BUFFER_BIT */);
            glFinish();
            unsigned char px[4] = {0,0,0,0};
            glReadPixels(2, 2, 1, 1, 0x1908 /*GL_RGBA*/, 0x1401 /*GL_UNSIGNED_BYTE*/, px);
            printf("  pixel[2,2] = %02x %02x %02x %02x\n", px[0], px[1], px[2], px[3]);
            if (px[0] == 0xff && px[1] == 0x00 && px[2] == 0xff)
                printf("*** MESA GL CLEAR OK IN BIONIC ***\n");
        }
    }
    return 0;
}

/* 看门狗：卡住时抓主线程 rip + 栈上指向我们模块的返回地址 */
static void on_alarm(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)si;
    ucontext_t *u = uc;
    unsigned long rip = 0, rsp = 0;
#ifdef __x86_64__
    rip = (unsigned long)u->uc_mcontext.gregs[REG_RIP];
    rsp = (unsigned long)u->uc_mcontext.gregs[REG_RSP];
#endif
    fprintf(stderr, "\n[watchdog] stuck in %s; rip=%p rsp=%p\n", g_phase_text, (void*)rip, (void*)rsp);
    for (int i = 0; i < g_nlibs; i++) {
        unsigned long b = (unsigned long)g_libs[i].base;
        if (rip >= b && rip < b + g_libs[i].mapsize)
            fprintf(stderr, "  rip in %s + 0x%lx\n", g_libs[i].name, rip - b);
    }
    const char *da = getenv("DUMP_AT");
    if (da) {
        unsigned long want = strtoul(da, NULL, 0);
        for (int i = 0; i < g_nlibs; i++) {
            if (strstr(g_libs[i].name, "amdxc64")) {
                unsigned long p = (unsigned long)g_libs[i].base + (want - g_libs[i].minv);
                fprintf(stderr, "  --- dump 0x%lx (lib+0x%lx) ---\n", p, want);
                unsigned char *m = (unsigned char*)p;
                for (int r = 0; r < 4; r++) {
                    fprintf(stderr, "   ");
                    for (int c = 0; c < 16; c++) fprintf(stderr, " %02x", m[r*16+c]);
                    fprintf(stderr, "\n");
                }
            }
        }
    }
    fprintf(stderr, "  --- stack scan (returns into loaded modules) ---\n");
    unsigned long *p = (unsigned long*)rsp;
    int shown = 0;
    for (int i = 0; i < 16384 && shown < 40; i++) {
        unsigned long v = p[i];
        if (v < 0x100000) continue;
        for (int k = 0; k < g_nlibs; k++) {
            unsigned long b = (unsigned long)g_libs[k].base;
            if (v >= b && v < b + g_libs[k].mapsize) {
                fprintf(stderr, "  [%4d] %p  %s + 0x%lx\n", i, (void*)v, g_libs[k].name, v - b);
                shown++;
                break;
            }
        }
    }
    _exit(3);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "direct0")) {
        void *h = dlopen("/data/local/tmp/libEGL_d3d12.so", RTLD_NOW | RTLD_GLOBAL);
        int (*init)(void) = h ? (int (*)(void))dlsym(h, "glibc_ldr_init") : NULL;
        int rc = init ? init() : -999;
        printf("  [direct0] h=%p rc=%d\n", h, rc);
        return rc;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    timing_begin();
    static char sigstack_mem[256 * 1024];
    stack_t ss; memset(&ss, 0, sizeof ss);
    ss.ss_sp = sigstack_mem; ss.ss_size = sizeof sigstack_mem; ss.ss_flags = 0;
    sigaltstack(&ss, NULL);
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = segv; sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    ldr_logw_init();
    if (env_on("SKIP_CXX_INIT")) g_skip_cxx_init = 1;
    load_skip_list();

    const char *stage = (argc > 1) ? argv[1] : "all";
    printf("== stage=%s ==\n", stage);
    if (getenv("SHOW_LIBC_SYMS"))
        printf("bionic: getenv=%p memcpy=%p strlen=%p printf=%p\n",
               dlsym(RTLD_DEFAULT, "getenv"), dlsym(RTLD_DEFAULT, "memcpy"),
               dlsym(RTLD_DEFAULT, "strlen"), dlsym(RTLD_DEFAULT, "printf"));

    if (!strcmp(stage, "direct1")) {
        void *h = dlopen("/data/local/tmp/libEGL_d3d12.so", RTLD_NOW | RTLD_GLOBAL);
        int (*init)(void) = h ? (int (*)(void))dlsym(h, "glibc_ldr_init") : NULL;
        int rc = init ? init() : -999;
        printf("  [direct1] h=%p rc=%d\n", h, rc);
        return rc;
    }
    if (!strcmp(stage, "dlprobe")) {
        void *h = dlopen("/data/local/tmp/libEGL_d3d12.so", RTLD_NOW | RTLD_GLOBAL);
        printf("  [dlprobe] dlopen=%p %s\n", h, h ? "" : dlerror());
        if (!h) return 1;
        int (*init)(void) = (int (*)(void))dlsym(h, "glibc_ldr_init");
        printf("  [dlprobe] init=%p\n", (void*)init);
        if (init) printf("  [dlprobe] init rc=%d\n", init());
        return 0;
    }
    lib_t *dx = load_elf("/data/local/tmp/libdxcore.so");
    run_inits(dx);
    lib_t *core = load_elf("/data/local/tmp/libd3d12core.so");
    run_inits(core);
    lib_t *ld = load_elf("/data/local/tmp/libd3d12.so");
    run_inits(ld);

    TLOG("== three libs loaded + inited ==");
    printf("== all inits done ==\n");
    if (!strcmp(stage, "init")) return 0;
    if (!strcmp(stage, "mesa")) {
        {
            struct sigaction wa; memset(&wa, 0, sizeof wa);
            wa.sa_sigaction = on_alarm; wa.sa_flags = SA_SIGINFO;
            sigaction(SIGALRM, &wa, NULL);
            const char *wd = getenv("WATCHDOG");
            if (wd && atoi(wd) > 0) alarm((unsigned)atoi(wd));
        }
        run_mesa_probe();
        return 0;
    }
    if (!strcmp(stage, "dxcore") || !strcmp(stage, "d3d12") || !strcmp(stage, "all")) {
        {
            struct sigaction wa; memset(&wa, 0, sizeof wa);
            wa.sa_sigaction = on_alarm; wa.sa_flags = SA_SIGINFO;
            sigaction(SIGALRM, &wa, NULL);
            const char *wd = getenv("WATCHDOG");
            if (wd && atoi(wd) > 0) alarm((unsigned)atoi(wd));
        }
        void *adapter = NULL, *dev = NULL;
        int rc = run_dxcore_probe(dx, ld, &adapter, &dev);
        if (rc != 0) printf("dxcore probe rc=%d\n", rc);
        if (!strcmp(stage, "dxcore")) return 0;
        if (!strcmp(stage, "d3d12")) {
            run_d3d12_smoke(ld, adapter, dev);
            return 0;
        }
    }

    void *addr = NULL;
    for (int i = 0; i < g_nlibs; i++) {
        addr = find_symbol_in(&g_libs[i], "D3D12CreateDevice");
        if (addr) { printf("D3D12CreateDevice in %s @ %p\n", g_libs[i].name, addr); break; }
    }
    if (!addr) return 1;
    d3d12_create_t f = (d3d12_create_t)addr;
    void *dev = NULL;
    g_phase = 1; g_phase_text = "D3D12CreateDevice";
    printf("calling D3D12CreateDevice(NULL, 0xb000, IID, &dev)...\n");
    long hr = f(NULL, 0xb000, &IID_ID3D12Device, &dev);
    printf("hr=0x%lx (%ld) dev=%p\n", (unsigned long)hr, hr, dev);
    return 0;
}

/* =====================================================================
 * 作为共享库使用时的入口（libEGL_d3d12.so）
 * 把上面这套 glibc 加载器包成一个可以在 bionic 进程里 dlopen 的库。
 * ===================================================================== */
static void *g_ldr_egl = NULL;
static void *g_ldr_dpy = NULL;
static int   g_ldr_rc = -1000;   /* -1000 未尝试；0 成功；<0 失败 */

static void ldr_install_signals(void) {
    static char sigstack_mem[256 * 1024];
    stack_t ss; memset(&ss, 0, sizeof ss);
    ss.ss_sp = sigstack_mem; ss.ss_size = sizeof sigstack_mem; ss.ss_flags = 0;
    sigaltstack(&ss, NULL);
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = segv; sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGILL,  &sa, NULL);
    sigaction(SIGFPE,  &sa, NULL);
}

/* ---- 诊断用：在 Mesa 库加载前后各建一次设备，看 UMD 是否只在有 Mesa 的进城里失败 ---- */
static void *g_ldr_adapter = NULL;
static void ldr_devprobe(const char *tag, lib_t *dx, lib_t *ld) {
    if (!ld) return;
    if (!g_ldr_adapter) {
        void *dev = NULL;
        run_dxcore_probe(dx, ld, &g_ldr_adapter, &dev);
        printf("  [probe:%s] adapter=%p dev=%p\n", tag, g_ldr_adapter, dev);
        if (!g_ldr_adapter) return;
    }
    long (*getif)(const GUID*, const GUID*, void**) =
        (long(*)(const GUID*,const GUID*,void**))find_symbol_in(ld, "D3D12GetInterface");
    if (!getif) { printf("  [probe:%s] no D3D12GetInterface\n", tag); return; }
    void *df = NULL;
    long h = getif(&CLSID_D3D12DeviceFactory, &IID_ID3D12DeviceFactory, &df);
    printf("  [probe:%s] GetInterface hr=0x%08lx df=%p\n", tag, (unsigned long)h, df);
    if (h || !df) return;
    h = ((long(*)(void*,unsigned))vtof(df)[5])(df, 3);
    printf("  [probe:%s] SetFlags(3) hr=0x%08lx\n", tag, (unsigned long)h);
    void *dv = NULL;
    h = ((long(*)(void*,void*,int,const GUID*,void**))vtof(df)[9])(df, g_ldr_adapter, 0x100, &IID_ID3D12Device3, &dv);
    printf("  [probe:%s] CreateDevice(FL=0x100) hr=0x%08lx dev=%p\n", tag, (unsigned long)h, dv);
}

/* zygote 里绝对不能初始化整套栈（详见 HANDOFF）：
 * ART 在 forkSystemServer 之前会走 ZygoteHooks.preFork -> Runtime::PreZygoteFork，
 * 那时 zygote 必须是单线程。我们一初始化就会拉起 UMD 并让 libd3d12core 建工作线程，
 * 于是 ART 直接 abort（status 134），zygote 反复重启、system_server 永远起不来。
 * 判定用 /proc/self/cmdline（zygote / zygote64 / zygote_secondary 都吃得到）。 */
static int proc_is_zygote(void) {
    static int v = -1;
    if (v >= 0) return v;
    v = 0;
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd >= 0) {
        char b[256];
        ssize_t n = read(fd, b, sizeof b - 1);
        close(fd);
        if (n > 0) { b[n] = 0; if (strstr(b, "zygote")) v = 1; }
    }
    return v;
}

/* 真实 Android 进程（SF/HWUI）不带我们的 HOME：容器的 / 是 overlay ro，
 * 而 AMD UMD 的 DXIL 缓存落在 $HOME/.cache/AMD —— 读到一个陈旧、写不回去的
 * 条目就会在 CreateDevice 里直接 0x887a0020 移除设备（第 22 轮定位）。
 * 所以入口处兜底：HOME 为空或不可写就指到 /data 下的可写目录。 */
static void ensure_writable_home(void) {
    const char *h = getenv("HOME");
    if (h && *h && access(h, W_OK) == 0) return;
    if (access("/data/local/tmp/home", W_OK) != 0) mkdir("/data/local/tmp/home", 0777);
    setenv("HOME", "/data/local/tmp/home", 1);
    fprintf(stderr, "[ldr] HOME -> /data/local/tmp/home (原值 %s)\n", (h && *h) ? h : "(空)");
}

__attribute__((visibility("default"))) int glibc_ldr_init(void) {
    if (g_ldr_rc != -1000) return g_ldr_rc;
    if (proc_is_zygote()) { g_ldr_rc = -7; return g_ldr_rc; }   /* 见 proc_is_zygote */
    ldr_logw_init();
    setvbuf(stdout, NULL, _IONBF, 0);
    ensure_writable_home();
    timing_begin();
    ldr_install_signals();
    if (env_on("SKIP_CXX_INIT")) g_skip_cxx_init = 1;
    load_skip_list();
    {
        const char *wd = getenv("WATCHDOG");
        if (wd && atoi(wd) > 0) {
            struct sigaction wa; memset(&wa, 0, sizeof wa);
            wa.sa_sigaction = on_alarm; wa.sa_flags = SA_SIGINFO;
            sigaction(SIGALRM, &wa, NULL);
            alarm((unsigned)atoi(wd));
        }
    }
    printf("== glibc_ldr_init: load dxcore/d3d12core/d3d12 ==\n");
    lib_t *dx   = load_elf("/data/local/tmp/libdxcore.so");
    if (dx)   run_inits(dx);
    lib_t *core = load_elf("/data/local/tmp/libd3d12core.so");
    if (core) run_inits(core);
    lib_t *d3d  = load_elf("/data/local/tmp/libd3d12.so");
    if (d3d)  run_inits(d3d);

    TLOG("== three libs inited ==");
    if (env_on("PROBE")) { double _t = now_ms(); ldr_devprobe("pre-mesa", dx, d3d); TLOG("pre-mesa probe took %.1f ms", now_ms() - _t); }

    char dir[256];
    snprintf(dir, sizeof dir, "%s", mesa_dir());
    char pbuf[600];
    snprintf(pbuf, sizeof pbuf, "%s/libgallium-26.0.8.so", dir);
    if (!load_so(pbuf)) { printf("mesa: libgallium load failed\n"); g_ldr_rc = -1; return g_ldr_rc; }
    snprintf(pbuf, sizeof pbuf, "%s/libEGL.so.1.0.0", dir);
    lib_t *egl = (lib_t *)load_so(pbuf);
    if (!egl) { printf("mesa: libEGL load failed\n"); g_ldr_rc = -2; return g_ldr_rc; }
    g_ldr_egl = egl;
    TLOG("mesa loaded (libgallium + libEGL)");

    if (env_on("PROBE")) { double _t = now_ms(); ldr_devprobe("post-mesa", dx, d3d); TLOG("post-mesa probe took %.1f ms", now_ms() - _t); }

    void *(*getPlat)(unsigned, void *, const long *) =
        (void *(*)(unsigned, void *, const long *))find_symbol_in(egl, "eglGetPlatformDisplay");
    int (*init)(void *, int *, int *) =
        (int (*)(void *, int *, int *))find_symbol_in(egl, "eglInitialize");
    if (!getPlat || !init) { printf("mesa: egl entry missing\n"); g_ldr_rc = -3; return g_ldr_rc; }
    g_phase = 500; g_phase_text = "eglGetPlatformDisplay";
    double _tegl = now_ms();
    void *dpy = getPlat(0x31DD /* EGL_PLATFORM_SURFACELESS_MESA */, NULL, NULL);
    TLOG("eglGetPlatformDisplay took %.1f ms dpy=%p", now_ms() - _tegl, dpy);
    if (!dpy) { printf("mesa: eglGetPlatformDisplay failed\n"); g_ldr_rc = -4; return g_ldr_rc; }
    int maj = 0, min = 0;
    _tegl = now_ms();
    int _ok = init(dpy, &maj, &min);
    TLOG("eglInitialize took %.1f ms ok=%d", now_ms() - _tegl, _ok);
    if (!_ok) { printf("mesa: eglInitialize failed\n"); g_ldr_rc = -5; return g_ldr_rc; }
    g_ldr_dpy = dpy;
    printf("== glibc_ldr_init OK dpy=%p EGL %d.%d ==\n", dpy, maj, min);
    g_ldr_rc = 0;
    return 0;
}

__attribute__((visibility("default"))) void *glibc_ldr_display(void) {
    if (g_ldr_rc == -1000) glibc_ldr_init();
    return g_ldr_dpy;
}

__attribute__((visibility("default"))) void *glibc_ldr_proc(const char *name) {
    if (g_ldr_rc == -1000) glibc_ldr_init();
    if (g_ldr_rc == -7) return NULL;   /* zygote：一律不提供 GL 入口 */
    if (!g_ldr_egl) return NULL;
    void *f = find_symbol_in((lib_t *)g_ldr_egl, name);
    if (f) return f;
    for (int i = 0; i < g_nlibs; i++) {
        f = find_symbol_in(&g_libs[i], name);
        if (f) return f;
    }
    return NULL;
}
