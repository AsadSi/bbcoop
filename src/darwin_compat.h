/* macOS (x86-64, Rosetta 2 on Apple Silicon): the Linux APIs the runtime uses that
 * Darwin lacks or names differently. Included through runtime.h. */
#ifndef BB_DARWIN_COMPAT_H
#define BB_DARWIN_COMPAT_H
#ifdef __APPLE__
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/uio.h>

/* struct stat timestamps */
#define st_atim st_atimespec
#define st_mtim st_mtimespec
#define st_ctim st_ctimespec

#ifndef CLOCK_REALTIME_COARSE
#define CLOCK_REALTIME_COARSE CLOCK_REALTIME
#endif
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif
#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME 1
#endif

/* Apple Silicon reserves [0x1000000000, 0x7000000000) for the GPU (the "GPU carveout"), also
 * in Rosetta processes. The low range (image, stacks, host objects) keeps Linux's addresses below
 * it, so the code that knows the image at 0x800000000 holds; the direct-memory range moves above. */
#define DARWIN_LOW_END UINT64_C(0x1000000000)
#define DARWIN_USER_MIN UINT64_C(0x7800000000)

static inline pid_t darwin_gettid(void) {
    uint64_t id = 0;
    pthread_threadid_np(NULL, &id);
    return (pid_t)id;
}
#define gettid darwin_gettid

static inline ssize_t darwin_getrandom(void *buffer, size_t size, unsigned flags) {
    (void)flags;
    arc4random_buf(buffer, size);
    return (ssize_t)size;
}
#define getrandom darwin_getrandom

/* Conditions wait on CLOCK_REALTIME only (RUNTIME_COND_CLOCK is that on macOS). */
#define pthread_condattr_setclock(attr, clock) ((void)(attr), (void)(clock), 0)

/* An anonymous file: a temporary one, unlinked at once. */
#define MFD_CLOEXEC 1u
static inline int darwin_memfd_create(const char *name, unsigned flags) {
    (void)name;
    char path[] = "/tmp/bb-memfd-XXXXXX";
    const int fd = mkstemp(path);
    if (fd < 0) return -1;
    unlink(path);
    if (flags & MFD_CLOEXEC) fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}
#define memfd_create darwin_memfd_create

/* Absolute sleep on CLOCK_MONOTONIC: the remaining time, slept relative. */
static inline int darwin_clock_nanosleep(clockid_t clock, int flags, const struct timespec *t, struct timespec *rest) {
    (void)rest;
    struct timespec now, d = *t;
    if (flags & TIMER_ABSTIME) {
        clock_gettime(clock, &now);
        d.tv_sec -= now.tv_sec; d.tv_nsec -= now.tv_nsec;
        if (d.tv_nsec < 0) { d.tv_nsec += 1000000000L; --d.tv_sec; }
        if (d.tv_sec < 0) return 0;
    }
    return nanosleep(&d, NULL) ? errno : 0;
}
#define clock_nanosleep darwin_clock_nanosleep

/* getrusage(RUSAGE_THREAD): the calling thread's CPU times. */
#define RUSAGE_THREAD 1
int darwin_getrusage(int who, struct rusage *r);
#define getrusage darwin_getrusage

/* Timed locks (realtime deadlines) Darwin's pthreads do not have: trylock and short sleeps. */
static inline int darwin_deadline_passed(const struct timespec *deadline) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return now.tv_sec > deadline->tv_sec || (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec);
}
static inline void darwin_lock_backoff(unsigned *spins) {
    if (++*spins < 64) { sched_yield(); return; }
    struct timespec pause = {0, 200000};
    nanosleep(&pause, NULL);
}
static inline int pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *deadline) {
    unsigned spins = 0;
    for (;;) {
        int e = pthread_mutex_trylock(m);
        if (e != EBUSY) return e;
        if (darwin_deadline_passed(deadline)) return ETIMEDOUT;
        darwin_lock_backoff(&spins);
    }
}
static inline int darwin_rwlock_timed(pthread_rwlock_t *l, int writer, const struct timespec *deadline) {
    unsigned spins = 0;
    for (;;) {
        int e = writer ? pthread_rwlock_trywrlock(l) : pthread_rwlock_tryrdlock(l);
        if (e != EBUSY) return e;
        if (darwin_deadline_passed(deadline)) return ETIMEDOUT;
        darwin_lock_backoff(&spins);
    }
}
#define pthread_rwlock_timedwrlock(l, d) darwin_rwlock_timed((l), 1, (d))
#define pthread_rwlock_timedrdlock(l, d) darwin_rwlock_timed((l), 0, (d))

/* pthread barriers (tests): Darwin's pthreads lack them. */
#ifndef PTHREAD_BARRIER_SERIAL_THREAD
#define PTHREAD_BARRIER_SERIAL_THREAD (-1)
typedef struct { pthread_mutex_t lock; pthread_cond_t cond; unsigned count, waiting, cycle; } pthread_barrier_t;
typedef int pthread_barrierattr_t;
static inline int pthread_barrier_init(pthread_barrier_t *b, const pthread_barrierattr_t *attr, unsigned count) {
    (void)attr;
    if (!count) return EINVAL;
    b->count = count; b->waiting = 0; b->cycle = 0;
    pthread_mutex_init(&b->lock, NULL);
    return pthread_cond_init(&b->cond, NULL);
}
static inline int pthread_barrier_destroy(pthread_barrier_t *b) {
    pthread_cond_destroy(&b->cond);
    return pthread_mutex_destroy(&b->lock);
}
static inline int pthread_barrier_wait(pthread_barrier_t *b) {
    pthread_mutex_lock(&b->lock);
    unsigned cycle = b->cycle;
    if (++b->waiting == b->count) {
        b->waiting = 0; ++b->cycle;
        pthread_cond_broadcast(&b->cond);
        pthread_mutex_unlock(&b->lock);
        return PTHREAD_BARRIER_SERIAL_THREAD;
    }
    while (cycle == b->cycle) pthread_cond_wait(&b->cond, &b->lock);
    pthread_mutex_unlock(&b->lock);
    return 0;
}
#endif

/* MAP_FIXED_NOREPLACE: an anonymous mapping exactly at the address or none (EEXIST), as on Linux;
 * VM_FLAGS_FIXED without OVERWRITE fails on an occupied range. */
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#define MAP_FIXED_NOREPLACE 0x40000000
static inline void *darwin_mmap(void *address, size_t size, int prot, int flags, int fd, off_t offset) {
    if (!(flags & MAP_FIXED_NOREPLACE)) return mmap(address, size, prot, flags, fd, offset);
    if (fd != -1 || !(flags & MAP_ANON)) { errno = EINVAL; return MAP_FAILED; }
    mach_vm_address_t at = (mach_vm_address_t)(uintptr_t)address;
    kern_return_t kr = mach_vm_allocate(mach_task_self(), &at, size, VM_FLAGS_FIXED);
    if (kr != KERN_SUCCESS) { errno = kr == KERN_NO_SPACE ? EEXIST : ENOMEM; return MAP_FAILED; }
    if (mprotect(address, size, prot)) { mach_vm_deallocate(mach_task_self(), at, size); return MAP_FAILED; }
    return address;
}
#define mmap darwin_mmap

/* Reads another part of this process without faulting (process_vm_readv below). */
ssize_t darwin_read_memory(void *out, const void *address, size_t size);
/* process_vm_readv on this process (fault handlers' stack reads): one buffer each side. */
static inline ssize_t process_vm_readv(pid_t pid, const struct iovec *local, unsigned long local_count,
                                       const struct iovec *remote, unsigned long remote_count, unsigned long flags) {
    (void)pid; (void)flags;
    if (local_count != 1 || remote_count != 1) return -1;
    return darwin_read_memory(local->iov_base, remote->iov_base,
                              local->iov_len < remote->iov_len ? local->iov_len : remote->iov_len);
}

/* Guest TCB in a pthread TSD slot: macOS points GS at the TSD array, so the loader
 * rewrites the eboot's `mov rax, fs:[0]` to `mov rax, gs:[slot*8]` (darwin_compat.c). */
unsigned runtime_darwin_tls_slot(void);
void runtime_darwin_set_tcb(void *tcb);
/* Rewrites every `mov rax, fs:[0]` / `mov rax, gs:[0]` in code to `mov rax, gs:[slot*8]` in
 * one pass; returns how many. */
uint64_t runtime_darwin_patch_tls_loads(unsigned char *code, size_t size);
#endif
#endif
