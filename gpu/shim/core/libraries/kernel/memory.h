// bbcoop: NpSignaling's private heap. Guest code runs natively, so host memory is guest memory.
#pragma once
#include <sys/mman.h>
#include "common/types.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/kernel/orbis_error.h"
namespace Libraries::Kernel {
inline s32 PS4_SYSV_ABI sceKernelMapNamedFlexibleMemory(void** addr_in_out, u64 len, s32 prot, s32 flags,
                                                        const char*) {
    (void)prot; (void)flags;
    void* p = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return ORBIS_KERNEL_ERROR_ENOMEM;
    *addr_in_out = p;
    return ORBIS_OK;
}
inline s32 PS4_SYSV_ABI sceKernelMunmap(void* addr, u64 len) {
    return munmap(addr, len) ? ORBIS_KERNEL_ERROR_EINVAL : ORBIS_OK;
}
} // namespace Libraries::Kernel
