/* bbcoop: code hooks on the game image (runtime_hooks.c). x86-64 Linux only. */
#ifndef BB_HOOKS_H
#define BB_HOOKS_H
#include <stdint.h>

/* The game's registers at the hook site. A handler may change any of them (rsp excepted); the
 * changes are in effect when the replaced instructions run. Field order matches hook_entry. */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8, rbp, rdi, rsi, rdx, rcx, rbx, rflags;
    uint64_t stub_return, rax;
} HookRegs;

typedef void (*HookHandler)(HookRegs *regs);

/* The game's rsp at the hook site (above the stub's slots and the 128-byte red zone). */
static inline uint64_t hook_guest_rsp(const HookRegs *regs) { return (uint64_t)(uintptr_t)(&regs->rax + 1) + 128; }

/* Reserves a stub area within rel32 reach of the image. Call once per set of hooks (seamless, respec;
 * each call maps its own area), after relocation and
 * while the image is still writable. Returns NULL or an error message. */
const char *hooks_init(unsigned char *image, uint64_t size);

/* Hooks the instructions at image offset `offset`. `original` must be the site's exact bytes,
 * at least 5 long, covering whole instructions with no relative operands (rel jumps/calls,
 * rip-relative memory): they are copied into the stub and run there after the handler.
 * Returns NULL or an error message; nothing is written on error. */
const char *hooks_install(uint64_t offset, const unsigned char *original, unsigned length, HookHandler handler);

/* Makes the stub area executable and read-only. Call after the last hooks_install. */
const char *hooks_seal(void);
#endif
