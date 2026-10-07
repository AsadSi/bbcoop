/* bbcoop: runtime_hooks.c on hand-assembled functions in an executable buffer (x86-64 Linux). */
#define _GNU_SOURCE
#include "hooks.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static unsigned char *image;
static int calls;
static volatile double sink;

/* Clobbers what a C handler normally may: vector registers, flags, plenty of stack. */
static void clobber(void) {
    volatile char scratch[4096];
    memset((char *)scratch, 0x5A, sizeof(scratch));
    double x = 1.0;
    for (int i = 0; i < 50; ++i) x = sqrt(x + i) * 1.5;
    sink = x;
}
static void set_rdi(HookRegs *regs) { ++calls; clobber(); regs->rdi = 100; }
static void observe(HookRegs *regs) { (void)regs; ++calls; clobber(); }
static void check_red_zone(HookRegs *regs) {
    ++calls;
    clobber();
    assert(*(uint64_t *)(uintptr_t)(hook_guest_rsp(regs) - 8) == 42);
}

static void put(uint64_t offset, const char *bytes, size_t length) { memcpy(image + offset, bytes, length); }

int main(void) {
    image = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(image != MAP_FAILED);
    memset(image, 0xCC, 0x10000);
    /* 0x100: rax = rdi + 5 */
    put(0x100, "\x48\x89\xF8" "\x48\x83\xC0\x05" "\xC3", 8);
    /* 0x200: rax = bits of xmm0 */
    put(0x200, "\x66\x48\x0F\x7E\xC0" "\xC3", 6);
    /* 0x300: stc; [site] mov rax, 0; adc rax, 0 -> 1 only if CF survives the hook */
    put(0x300, "\xF9" "\x48\xC7\xC0\x00\x00\x00\x00" "\x48\x83\xD0\x00" "\xC3", 13);
    /* 0x400: [rsp-8] = 42; [site] mov rax, rdi; mov rax, rax; rax = [rsp-8] */
    put(0x400, "\x48\xC7\x44\x24\xF8\x2A\x00\x00\x00" "\x48\x89\xF8\x48\x89\xC0" "\x48\x8B\x44\x24\xF8" "\xC3", 21);

    assert(hooks_install(0x100, (const unsigned char *)"\x48\x89\xF8\x48\x83\xC0\x05", 7, set_rdi) != NULL); /* before init */
    assert(hooks_init(image, 0x10000) == NULL);

    /* Wrong bytes, short sites and out-of-range sites: refused, nothing written. */
    unsigned char before[0x500];
    memcpy(before, image, sizeof(before));
    assert(strstr(hooks_install(0x100, (const unsigned char *)"\x48\x89\xF8\x48\x83\xC0\x06", 7, set_rdi), "do not match"));
    assert(hooks_install(0x100, (const unsigned char *)"\x48\x89\xF8", 3, set_rdi) != NULL);
    assert(hooks_install(0xFFFE, (const unsigned char *)"\xCC\xCC\xCC\xCC\xCC", 5, set_rdi) != NULL);
    assert(!memcmp(before, image, sizeof(before)));

    assert(hooks_install(0x100, (const unsigned char *)"\x48\x89\xF8\x48\x83\xC0\x05", 7, set_rdi) == NULL);
    assert(hooks_install(0x100, (const unsigned char *)"\x48\x89\xF8\x48\x83\xC0\x05", 7, set_rdi) != NULL); /* twice */
    assert(hooks_install(0x200, (const unsigned char *)"\x66\x48\x0F\x7E\xC0", 5, observe) == NULL);
    assert(hooks_install(0x301, (const unsigned char *)"\x48\xC7\xC0\x00\x00\x00\x00", 7, observe) == NULL);
    assert(hooks_install(0x409, (const unsigned char *)"\x48\x89\xF8\x48\x89\xC0", 6, check_red_zone) == NULL);
    assert(hooks_seal() == NULL);

    /* The handler's register change takes effect before the replaced instructions run. */
    assert(((uint64_t (*)(uint64_t))(image + 0x100))(1) == 105);
    /* xmm0 comes through a handler that uses floating point. */
    double value = 3.25;
    uint64_t bits;
    memcpy(&bits, &value, 8);
    assert(((uint64_t (*)(double))(image + 0x200))(value) == bits);
    /* Flags come through. */
    assert(((uint64_t (*)(void))(image + 0x300))() == 1);
    /* The red zone below the game's rsp is untouched, and the handler sees the game's rsp. */
    assert(((uint64_t (*)(uint64_t))(image + 0x400))(7) == 42);
    assert(calls == 4);
    /* Hooks run every time. */
    assert(((uint64_t (*)(uint64_t))(image + 0x100))(9) == 105 && calls == 5);
    puts("hooks: ok");
    return 0;
}
