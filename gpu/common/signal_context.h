// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: what a SIGSEGV handler needs from its ucontext (x86-64 Linux, or macOS under Rosetta 2).
#pragma once

#include <sys/ucontext.h>

#include "common/types.h"

namespace Common {

/// The faulting instruction.
inline void* GetRip(void* context) {
#ifdef __APPLE__
    return reinterpret_cast<void*>(static_cast<ucontext_t*>(context)->uc_mcontext->__ss.__rip);
#else
    return reinterpret_cast<void*>(static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP]);
#endif
}

/// The fault was a write (page fault error code bit 1).
inline bool IsWriteError(void* context) {
#ifdef __APPLE__
    return static_cast<ucontext_t*>(context)->uc_mcontext->__es.__err & 0x2;
#else
    return static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_ERR] & 0x2;
#endif
}

} // namespace Common
