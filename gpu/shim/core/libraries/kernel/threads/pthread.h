// bbcoop: shadPS4's guest pthread API, as used by NpCommon's internal threads and locks, on host
// pthreads. The game never sees these objects (it imports no NpCommon thread/mutex functions).
// Threads attach to the C runtime first, so they can run guest callbacks (GS base, TLS).
#pragma once
#include <cerrno>
#include <ctime>
#include <pthread.h>
#include "common/types.h"
#include "core/libraries/kernel/time.h"

extern "C" void runtime_thread_attach_host(const char* name);

namespace Libraries::Kernel {

constexpr int ORBIS_KERNEL_PRIO_FIFO_DEFAULT = 700;
enum class PthreadMutexType : u32 { ErrorCheck = 1, Recursive = 2, Normal = 3, AdaptiveNp = 4, Max };
enum class SchedPolicy : u32 { Fifo = 1, Other = 2, RoundRobin = 3 };
struct SchedParam {
    int sched_priority;
};

struct Pthread {
    pthread_t thread;
};
struct PthreadAttr {
    size_t stack_size;
};
using PthreadT = Pthread*;
using PthreadAttrT = PthreadAttr*;
using PthreadMutexT = pthread_mutex_t*;
using PthreadMutexAttrT = pthread_mutexattr_t*;
using PthreadCondT = pthread_cond_t*;
using PthreadCondAttrT = pthread_condattr_t*;
using PthreadEntryFunc = void* PS4_SYSV_ABI (*)(void*);

inline int PS4_SYSV_ABI posix_pthread_mutexattr_init(PthreadMutexAttrT* attr) {
    *attr = new pthread_mutexattr_t;
    return pthread_mutexattr_init(*attr);
}
inline int PS4_SYSV_ABI posix_pthread_mutexattr_settype(PthreadMutexAttrT* attr, PthreadMutexType type) {
    return pthread_mutexattr_settype(*attr, type == PthreadMutexType::Recursive ? PTHREAD_MUTEX_RECURSIVE
                                            : type == PthreadMutexType::ErrorCheck ? PTHREAD_MUTEX_ERRORCHECK
                                                                                   : PTHREAD_MUTEX_NORMAL);
}
inline int PS4_SYSV_ABI posix_pthread_mutexattr_destroy(PthreadMutexAttrT* attr) {
    int rc = pthread_mutexattr_destroy(*attr);
    delete *attr;
    *attr = nullptr;
    return rc;
}
inline int PS4_SYSV_ABI scePthreadMutexInit(PthreadMutexT* mutex, const PthreadMutexAttrT* attr, const char*) {
    *mutex = new pthread_mutex_t;
    return pthread_mutex_init(*mutex, attr ? *attr : nullptr);
}
inline int PS4_SYSV_ABI posix_pthread_mutex_destroy(PthreadMutexT* mutex) {
    int rc = pthread_mutex_destroy(*mutex);
    delete *mutex;
    *mutex = nullptr;
    return rc;
}
inline int PS4_SYSV_ABI posix_pthread_mutex_lock(PthreadMutexT* mutex) { return pthread_mutex_lock(*mutex); }
inline int PS4_SYSV_ABI posix_pthread_mutex_trylock(PthreadMutexT* mutex) { return pthread_mutex_trylock(*mutex); }
inline int PS4_SYSV_ABI posix_pthread_mutex_unlock(PthreadMutexT* mutex) { return pthread_mutex_unlock(*mutex); }

inline int PS4_SYSV_ABI posix_pthread_condattr_init(PthreadCondAttrT* attr) {
    *attr = new pthread_condattr_t;
    pthread_condattr_init(*attr);
    return pthread_condattr_setclock(*attr, CLOCK_MONOTONIC);
}
inline int PS4_SYSV_ABI posix_pthread_condattr_destroy(PthreadCondAttrT* attr) {
    int rc = pthread_condattr_destroy(*attr);
    delete *attr;
    *attr = nullptr;
    return rc;
}
inline int PS4_SYSV_ABI scePthreadCondInit(PthreadCondT* cond, const PthreadCondAttrT* attr, const char*) {
    *cond = new pthread_cond_t;
    return pthread_cond_init(*cond, attr ? *attr : nullptr);
}
inline int PS4_SYSV_ABI posix_pthread_cond_destroy(PthreadCondT* cond) {
    int rc = pthread_cond_destroy(*cond);
    delete *cond;
    *cond = nullptr;
    return rc;
}
inline int PS4_SYSV_ABI posix_pthread_cond_signal(PthreadCondT* cond) { return pthread_cond_signal(*cond); }
inline int PS4_SYSV_ABI posix_pthread_cond_wait(PthreadCondT* cond, PthreadMutexT* mutex) {
    return pthread_cond_wait(*cond, *mutex);
}
/* The condition must have been made by scePthreadCondInit with posix_pthread_condattr_init's
 * attribute (CLOCK_MONOTONIC), as sceNpCondInit does. */
inline int PS4_SYSV_ABI posix_pthread_cond_reltimedwait_np(PthreadCondT* cond, PthreadMutexT* mutex, u64 usec) {
    timespec at;
    clock_gettime(CLOCK_MONOTONIC, &at);
    at.tv_sec += time_t(usec / 1000000);
    at.tv_nsec += long(usec % 1000000) * 1000;
    if (at.tv_nsec >= 1000000000) { at.tv_sec += 1; at.tv_nsec -= 1000000000; }
    return pthread_cond_timedwait(*cond, *mutex, &at);
}

/* Scheduling requests (FIFO priority, affinity) are accepted and ignored on the PC. */
inline int PS4_SYSV_ABI posix_pthread_attr_init(PthreadAttrT* attr) {
    *attr = new PthreadAttr{};
    return 0;
}
inline int PS4_SYSV_ABI posix_pthread_attr_destroy(PthreadAttrT* attr) {
    delete *attr;
    *attr = nullptr;
    return 0;
}
inline int PS4_SYSV_ABI posix_pthread_attr_setstacksize(PthreadAttrT* attr, size_t size) {
    (*attr)->stack_size = size;
    return 0;
}
inline int PS4_SYSV_ABI posix_pthread_attr_setinheritsched(PthreadAttrT*, int) { return 0; }
inline int PS4_SYSV_ABI posix_pthread_attr_setschedpolicy(PthreadAttrT*, SchedPolicy) { return 0; }
inline int PS4_SYSV_ABI posix_pthread_attr_setschedparam(PthreadAttrT*, SchedParam*) { return 0; }
inline int PS4_SYSV_ABI scePthreadAttrSetaffinity(PthreadAttrT*, const u64) { return 0; }

namespace Detail {
struct ThreadStart {
    PthreadEntryFunc entry;
    void* arg;
    char name[32];
};
inline void* ThreadMain(void* p) {
    ThreadStart start = *static_cast<ThreadStart*>(p);
    delete static_cast<ThreadStart*>(p);
    runtime_thread_attach_host(start.name);
    return start.entry(start.arg);
}
} // namespace Detail

inline int PS4_SYSV_ABI posix_pthread_create_name_np(PthreadT* thread, const PthreadAttrT* attr,
                                                     PthreadEntryFunc entry, void* arg, const char* name) {
    auto* start = new Detail::ThreadStart{entry, arg, {}};
    snprintf(start->name, sizeof(start->name), "%s", name ? name : "bb:np");
    pthread_attr_t host;
    pthread_attr_init(&host);
    if (attr && *attr && (*attr)->stack_size >= PTHREAD_STACK_MIN)
        pthread_attr_setstacksize(&host, (*attr)->stack_size);
    *thread = new Pthread{};
    int rc = pthread_create(&(*thread)->thread, &host, Detail::ThreadMain, start);
    pthread_attr_destroy(&host);
    if (rc) {
        delete start;
        delete *thread;
        *thread = nullptr;
    }
    return rc;
}
inline int PS4_SYSV_ABI posix_pthread_join(PthreadT thread, void** ret) {
    int rc = pthread_join(thread->thread, ret);
    delete thread;
    return rc;
}

} // namespace Libraries::Kernel
