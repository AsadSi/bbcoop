/* bbcoop: code hooks on the game image. A hooked site becomes `jmp stub` (+ NOP fill); the stub
 * steps over the red zone, saves every register (GPRs, flags, ymm0-15, MXCSR) through
 * bbcoop_hook_entry, calls the C handler with them, restores them, runs the site's original
 * instructions and jumps back. Same scheme as shadp2p's InstallGuestCodeHook
 * (src/core/cpu_patches.cpp), without a code generator: the stub is a fixed byte template. */
#if defined(__x86_64__) && (defined(__linux__) || defined(__APPLE__) || defined(_WIN32)) /* bbcoop: + Windows */
#define _GNU_SOURCE
#include "darwin_compat.h" /* bbcoop: MAP_FIXED_NOREPLACE on macOS */
#include "hooks.h"
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#define STUB_AREA (64 * 1024)
#define MAX_SITE 32

static unsigned char *image_base, *stubs, *stub_next;
static uint64_t image_size;
static char error[160];

extern void bbcoop_hook_entry(void);
/* Entry: [rsp] = return into the stub, [rsp+8] = the game's rax, rax = handler. The pushes
 * below lay the registers out as HookRegs, which is passed to the handler and popped back. */
__asm__(
    ".text\n"
#ifdef __APPLE__
    ".globl _bbcoop_hook_entry\n"
    ".private_extern _bbcoop_hook_entry\n"
    "_bbcoop_hook_entry:\n"
#elif defined(_WIN32)
    ".globl bbcoop_hook_entry\n"
    ".def bbcoop_hook_entry; .scl 2; .type 32; .endef\n"
    "bbcoop_hook_entry:\n"
#else
    ".globl bbcoop_hook_entry\n"
    ".hidden bbcoop_hook_entry\n"
    ".type bbcoop_hook_entry, @function\n"
    "bbcoop_hook_entry:\n"
#endif
    "    pushfq\n"
    "    push %rbx\n push %rcx\n push %rdx\n push %rsi\n push %rdi\n push %rbp\n push %r8\n"
    "    push %r9\n push %r10\n push %r11\n push %r12\n push %r13\n push %r14\n push %r15\n"
    "    mov %rsp, %rbx\n"
    "    sub $544, %rsp\n"
    "    and $-32, %rsp\n"
    "    vmovdqu %ymm0, 0(%rsp)\n    vmovdqu %ymm1, 32(%rsp)\n    vmovdqu %ymm2, 64(%rsp)\n"
    "    vmovdqu %ymm3, 96(%rsp)\n   vmovdqu %ymm4, 128(%rsp)\n   vmovdqu %ymm5, 160(%rsp)\n"
    "    vmovdqu %ymm6, 192(%rsp)\n  vmovdqu %ymm7, 224(%rsp)\n   vmovdqu %ymm8, 256(%rsp)\n"
    "    vmovdqu %ymm9, 288(%rsp)\n  vmovdqu %ymm10, 320(%rsp)\n  vmovdqu %ymm11, 352(%rsp)\n"
    "    vmovdqu %ymm12, 384(%rsp)\n vmovdqu %ymm13, 416(%rsp)\n  vmovdqu %ymm14, 448(%rsp)\n"
    "    vmovdqu %ymm15, 480(%rsp)\n"
    "    stmxcsr 512(%rsp)\n"
    "    cld\n"
#ifdef _WIN32
    /* Windows x64: the argument in rcx and 32 bytes of shadow space for the handler. */
    "    mov %rbx, %rcx\n"
    "    sub $32, %rsp\n"
    "    call *%rax\n"
    "    add $32, %rsp\n"
#else
    "    mov %rbx, %rdi\n"
    "    call *%rax\n"
#endif
    "    ldmxcsr 512(%rsp)\n"
    "    vmovdqu 0(%rsp), %ymm0\n    vmovdqu 32(%rsp), %ymm1\n    vmovdqu 64(%rsp), %ymm2\n"
    "    vmovdqu 96(%rsp), %ymm3\n   vmovdqu 128(%rsp), %ymm4\n   vmovdqu 160(%rsp), %ymm5\n"
    "    vmovdqu 192(%rsp), %ymm6\n  vmovdqu 224(%rsp), %ymm7\n   vmovdqu 256(%rsp), %ymm8\n"
    "    vmovdqu 288(%rsp), %ymm9\n  vmovdqu 320(%rsp), %ymm10\n  vmovdqu 352(%rsp), %ymm11\n"
    "    vmovdqu 384(%rsp), %ymm12\n vmovdqu 416(%rsp), %ymm13\n  vmovdqu 448(%rsp), %ymm14\n"
    "    vmovdqu 480(%rsp), %ymm15\n"
    "    mov %rbx, %rsp\n"
    "    pop %r15\n pop %r14\n pop %r13\n pop %r12\n pop %r11\n pop %r10\n pop %r9\n pop %r8\n"
    "    pop %rbp\n pop %rdi\n pop %rsi\n pop %rdx\n pop %rcx\n pop %rbx\n"
    "    popfq\n"
    "    ret\n"
#if !defined(__APPLE__) && !defined(_WIN32)
    ".size bbcoop_hook_entry, .-bbcoop_hook_entry\n"
#endif
    );

static const char *failure(const char *message, uint64_t offset) {
    snprintf(error, sizeof(error), "hook at %#llx: %s", (unsigned long long)offset, message);
    return error;
}

static int within_rel32(const unsigned char *from, const unsigned char *to) {
    int64_t distance = to - from;
    return distance >= INT32_MIN && distance <= INT32_MAX;
}

const char *hooks_init(unsigned char *image, uint64_t size) {
    image_base = image;
    image_size = size;
    /* First free 64 KiB above the image, scanning 1 MiB steps up to 1 GiB past its end. */
    uintptr_t start = ((uintptr_t)image + size + 0xFFFFF) & ~(uintptr_t)0xFFFFF;
    for (uintptr_t at = start; at < start + (UINT64_C(1) << 30); at += 0x100000) {
        void *p = mmap((void *)at, STUB_AREA, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p == MAP_FAILED) continue;
        if (p != (void *)at) { munmap(p, STUB_AREA); continue; } /* kernels without NOREPLACE */
        stubs = stub_next = p;
        return NULL;
    }
    return "hooks: no free memory within 1 GiB above the game image";
}

const char *hooks_install(uint64_t offset, const unsigned char *original, unsigned length, HookHandler handler) {
    if (!stubs) return failure("hooks_init was not called", offset);
    if (length < 5 || length > MAX_SITE) return failure("site must be 5 to 32 bytes", offset);
    if (offset > image_size || length > image_size - offset) return failure("site outside the image", offset);
    unsigned char *site = image_base + offset;
    if (memcmp(site, original, length)) return failure("bytes do not match CUSA03173 01.09", offset);
    if (site[0] == 0xE9) return failure("site is already hooked", offset);

    unsigned char stub[64 + MAX_SITE], *p = stub;
    uint64_t value;
    memcpy(p, "\x48\x8D\x64\x24\x80", 5); p += 5;           /* lea rsp, [rsp-128] */
    *p++ = 0x50;                                              /* push rax */
    memcpy(p, "\x48\xB8", 2); p += 2;                         /* mov rax, handler */
    value = (uint64_t)(uintptr_t)handler; memcpy(p, &value, 8); p += 8;
    memcpy(p, "\xFF\x15\x02\x00\x00\x00", 6); p += 6;        /* call [rip+2] */
    memcpy(p, "\xEB\x08", 2); p += 2;                         /* jmp over the pointer */
    value = (uint64_t)(uintptr_t)bbcoop_hook_entry; memcpy(p, &value, 8); p += 8;
    *p++ = 0x58;                                              /* pop rax */
    memcpy(p, "\x48\x8D\xA4\x24\x80\x00\x00\x00", 8); p += 8; /* lea rsp, [rsp+128] */
    memcpy(p, original, length); p += length;                 /* the replaced instructions */
    memcpy(p, "\xFF\x25\x00\x00\x00\x00", 6); p += 6;        /* jmp [rip+0] */
    value = (uint64_t)(uintptr_t)(site + length); memcpy(p, &value, 8); p += 8;

    size_t stub_size = (size_t)(p - stub);
    if ((size_t)(stubs + STUB_AREA - stub_next) < stub_size) return failure("stub area full", offset);
    if (!within_rel32(site + 5, stub_next)) return failure("stub out of rel32 reach", offset);
    memcpy(stub_next, stub, stub_size);

    int32_t rel = (int32_t)(stub_next - (site + 5));
    site[0] = 0xE9;
    memcpy(site + 1, &rel, 4);
    memset(site + 5, 0x90, length - 5);
    stub_next += (stub_size + 15) & ~(size_t)15;
    return NULL;
}

const char *hooks_seal(void) {
    if (stubs && mprotect(stubs, STUB_AREA, PROT_READ | PROT_EXEC)) return "hooks: mprotect failed";
    return NULL;
}
#endif
