// SPDX-License-Identifier: GPL-2.0-or-later
// bbport on macOS (x86-64 under Rosetta 2): the Linux calls the GPU library makes that Darwin
// lacks, for every source of libbbgpu (CMake passes it with -include on Apple only).

#pragma once
#ifdef __APPLE__

#include <errno.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

/// The kernel's id of the calling thread (Linux gettid).
static inline pid_t gettid(void) {
    uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return static_cast<pid_t>(id);
}

/// process_vm_readv on this process: reads that may hit unmapped memory return what was read
/// before it, without a fault.
static inline ssize_t process_vm_readv(pid_t pid, const struct iovec* local, unsigned long local_count,
                                       const struct iovec* remote, unsigned long remote_count,
                                       unsigned long flags) {
    (void)flags;
    if (pid != getpid() || local_count != 1 || remote_count != 1) {
        return -1;
    }
    mach_vm_size_t size = local->iov_len < remote->iov_len ? local->iov_len : remote->iov_len;
    mach_vm_size_t got = 0;
    if (mach_vm_read_overwrite(mach_task_self(), reinterpret_cast<mach_vm_address_t>(remote->iov_base),
                               size, reinterpret_cast<mach_vm_address_t>(local->iov_base),
                               &got) != KERN_SUCCESS) {
        return -1;
    }
    return static_cast<ssize_t>(got);
}

/// pthread_setname_np(thread, name): Darwin names the calling thread only.
static inline int pthread_setname_np(pthread_t thread, const char* name) {
    return pthread_equal(thread, pthread_self()) ? pthread_setname_np(name) : 0;
}

/// MAP_FIXED_NOREPLACE: Darwin takes the address as a hint; a mapping that lands elsewhere is
/// released and fails with EEXIST, as on Linux (the hook stubs must stay near the game image).
#define MAP_FIXED_NOREPLACE 0x40000000
static inline void* bb_darwin_mmap(void* address, size_t size, int prot, int flags, int fd, off_t offset) {
    if (!(flags & MAP_FIXED_NOREPLACE)) {
        return mmap(address, size, prot, flags, fd, offset);
    }
    void* p = mmap(address, size, prot, flags & ~MAP_FIXED_NOREPLACE, fd, offset);
    if (p != MAP_FAILED && p != address) {
        munmap(p, size);
        errno = EEXIST;
        return MAP_FAILED;
    }
    return p;
}
#define mmap bb_darwin_mmap

/// memfd_create: Darwin has none; the callers fall back as when the call fails on Linux.
#define MFD_CLOEXEC 1U
static inline int memfd_create(const char*, unsigned) {
    errno = ENOSYS;
    return -1;
}

#endif
