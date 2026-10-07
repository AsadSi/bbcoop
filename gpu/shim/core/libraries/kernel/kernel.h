// bbport: the parts of shadPS4's kernel.h used by vendored libraries.
#pragma once
#include <cstdio>
#include "common/types.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/kernel/orbis_error.h"
namespace Libraries::Kernel {
int* PS4_SYSV_ABI __Error();
s32 PS4_SYSV_ABI sceKernelGetCompiledSdkVersion(s32* ver); // bbgpu.cpp

// bbcoop: used by the NP and Net libraries (gpu/shadps4/core/libraries/{np,network}).
#define CURRENT_FIRMWARE_VERSION 0x13520001
inline s32 ErrnoToSceKernelError(s32 error) { return error + ORBIS_KERNEL_ERROR_UNKNOWN; }
struct SwVersionStruct {
    u64 struct_size;
    char text_representation[0x1c];
    u32 hex_representation;
};
inline s32 PS4_SYSV_ABI sceKernelGetSystemSwVersion(SwVersionStruct* ret) {
    if (!ret) return ORBIS_OK;
    const u32 fw = CURRENT_FIRMWARE_VERSION;
    ret->hex_representation = fw;
    std::snprintf(ret->text_representation, 28, "%2x.%03x.%03x", fw >> 0x18, fw >> 0xc & 0xfff, fw & 0xfff);
    return ORBIS_OK;
}
} // namespace Libraries::Kernel
