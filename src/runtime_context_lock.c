/* bbcoop: one lock around the two engine paths that write the immediate graphics context (the load
 * race behind "PM4 type 0", docs/brief.md). Taken from Wooloomooloo2/bloodborne_mac_port 12a0ac5
 * (src/runtime_hooks.c there, Mark Hall, GPL-2.0), with both sites checked before either is written.
 *
 * During a load the main thread records cloth/texture setup dispatches (0xebb960) straight into
 * GXRenderThread's immediate context, with no lock: on the PS4 that step ends before the render
 * thread's next vblank-paced frame job (0x2199af0) starts. Under Rosetta the step can outlast the
 * vblank, both threads then write the same command buffer and the GPU gets interleaved packets.
 * Both functions run under one lock, so each waits for the other. A wait gives up after
 * LOCK_TIMEOUT_MS (reported) and runs the function anyway: a hidden dependency between the two
 * must not hang the game.
 *
 * A hooked function's first instructions (position-independent, checked byte for byte) move into a
 * stub that jumps back to the rest of the function; its entry becomes an absolute jump to the
 * common hook, which takes the lock, calls the stub and releases the lock. Neither function takes
 * stack arguments, so the extra return address does not move any. BB_CONTEXT_LOCK=0 leaves the
 * game unpatched. */
#define _GNU_SOURCE
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#define LOCK_TIMEOUT_MS 100
#define MAX_REPORTS 24

typedef struct Hook {
    void *original; /* the moved prologue + jump back: read by the asm at offset 0 */
    const char *name;
    uint64_t offset; /* image offset of the function */
    unsigned char prologue[24];
    unsigned prologue_size;
    _Atomic uint64_t calls, waits, wait_ns, timeouts;
} Hook;

static Hook hooks[] = {
    /* push rbp; mov rbp,rsp; push r15; push r14; push r13; push r12; push rbx; sub rsp,0x58 */
    {NULL, "main thread's GPU setup", 0xebb960,
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x58},
     17, 0, 0, 0, 0},
    /* push rbp; mov rbp,rsp; push r15; push r14; push rbx; sub rsp,0x38; mov rbx,rsi */
    {NULL, "GXRenderThread frame", 0x2199af0,
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x38, 0x48, 0x89, 0xf3},
     16, 0, 0, 0, 0},
};
#define HOOK_COUNT (sizeof(hooks) / sizeof(hooks[0]))

static pthread_mutex_t lock_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t lock_cond = PTHREAD_COND_INITIALIZER;
static pthread_t lock_holder;
static int lock_held;
static unsigned lock_depth;
static const Hook *lock_holder_hook;
static _Atomic unsigned reports;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* Tokens: 0 = nothing to release (timed out), 1 = taken, 2 = taken again by its holder. */
uint64_t bbcoop_context_lock_enter(Hook *hook);
uint64_t bbcoop_context_lock_enter(Hook *hook) {
    atomic_fetch_add_explicit(&hook->calls, 1, memory_order_relaxed);
    pthread_t self = pthread_self();
    pthread_mutex_lock(&lock_mutex);
    if (lock_held && pthread_equal(lock_holder, self)) {
        ++lock_depth;
        pthread_mutex_unlock(&lock_mutex);
        return 2;
    }
    uint64_t token = 1;
    if (lock_held) {
        const Hook *other = lock_holder_hook;
        uint64_t start = now_ns(), deadline = start + (uint64_t)LOCK_TIMEOUT_MS * 1000000u;
        while (lock_held) {
            uint64_t now = now_ns();
            if (now >= deadline) break;
            uint64_t left = deadline - now;
            struct timespec wait = {(time_t)(left / 1000000000u), (long)(left % 1000000000u)};
#ifdef __APPLE__
            pthread_cond_timedwait_relative_np(&lock_cond, &lock_mutex, &wait);
#else
            struct timespec abs;
            clock_gettime(CLOCK_REALTIME, &abs);
            abs.tv_sec += wait.tv_sec;
            abs.tv_nsec += wait.tv_nsec;
            if (abs.tv_nsec >= 1000000000) { abs.tv_sec++; abs.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&lock_cond, &lock_mutex, &abs);
#endif
        }
        uint64_t waited = now_ns() - start;
        atomic_fetch_add_explicit(&hook->waits, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&hook->wait_ns, waited, memory_order_relaxed);
        if (lock_held) {
            token = 0;
            atomic_fetch_add_explicit(&hook->timeouts, 1, memory_order_relaxed);
        }
        if (atomic_fetch_add_explicit(&reports, 1, memory_order_relaxed) < MAX_REPORTS) {
            fprintf(stderr, "Context lock: %s waited %.2f ms for %s%s (calls %" PRIu64 ", waits %" PRIu64 ")\n",
                    hook->name, (double)waited / 1e6, other ? other->name : "?",
                    token ? "" : ": timed out, running anyway",
                    atomic_load_explicit(&hook->calls, memory_order_relaxed),
                    atomic_load_explicit(&hook->waits, memory_order_relaxed));
        }
    }
    if (token) {
        lock_held = 1;
        lock_holder = self;
        lock_depth = 1;
        lock_holder_hook = hook;
    }
    pthread_mutex_unlock(&lock_mutex);
    return token;
}

void bbcoop_context_lock_leave(Hook *hook, uint64_t token);
void bbcoop_context_lock_leave(Hook *hook, uint64_t token) {
    (void)hook;
    if (!token) return;
    pthread_mutex_lock(&lock_mutex);
    if (lock_held && pthread_equal(lock_holder, pthread_self()) && --lock_depth == 0) {
        lock_held = 0;
        lock_holder_hook = NULL;
        pthread_cond_broadcast(&lock_cond);
    }
    pthread_mutex_unlock(&lock_mutex);
}

#ifdef __APPLE__
#define SYM(name) "_" #name
#else
#define SYM(name) #name
#endif

/* Entry: r11 = Hook*, the guest's return address on the stack (rsp = 8 mod 16). Saves the argument
 * registers around the enter call, calls the original through Hook.original (no stack arguments to
 * move), keeps the return registers around the leave call. */
__asm__(".text\n"
        ".p2align 4\n" SYM(bbcoop_context_lock_common) ":\n"
        "    pushq %rbp\n"
        "    movq %rsp, %rbp\n"
        "    pushq %r11\n" /* -8: hook */
        "    pushq %rax\n" /* -16: token (al: vector count of a variadic call) */
        "    pushq %rdi\n"
        "    pushq %rsi\n"
        "    pushq %rdx\n"
        "    pushq %rcx\n"
        "    pushq %r8\n"
        "    pushq %r9\n"
        "    subq $128, %rsp\n"
        "    movdqu %xmm0, 0(%rsp)\n"
        "    movdqu %xmm1, 16(%rsp)\n"
        "    movdqu %xmm2, 32(%rsp)\n"
        "    movdqu %xmm3, 48(%rsp)\n"
        "    movdqu %xmm4, 64(%rsp)\n"
        "    movdqu %xmm5, 80(%rsp)\n"
        "    movdqu %xmm6, 96(%rsp)\n"
        "    movdqu %xmm7, 112(%rsp)\n"
        "    movq %r11, %rdi\n"
        "    callq " SYM(bbcoop_context_lock_enter) "\n"
        "    movq -16(%rbp), %r11\n" /* the caller's rax back, the token in its slot */
        "    movq %rax, -16(%rbp)\n"
        "    movq %r11, %rax\n"
        "    movdqu 0(%rsp), %xmm0\n"
        "    movdqu 16(%rsp), %xmm1\n"
        "    movdqu 32(%rsp), %xmm2\n"
        "    movdqu 48(%rsp), %xmm3\n"
        "    movdqu 64(%rsp), %xmm4\n"
        "    movdqu 80(%rsp), %xmm5\n"
        "    movdqu 96(%rsp), %xmm6\n"
        "    movdqu 112(%rsp), %xmm7\n"
        "    addq $128, %rsp\n"
        "    popq %r9\n"
        "    popq %r8\n"
        "    popq %rcx\n"
        "    popq %rdx\n"
        "    popq %rsi\n"
        "    popq %rdi\n" /* rsp = rbp - 16: 0 mod 16 */
        "    movq -8(%rbp), %r11\n"
        "    callq *(%r11)\n"
        "    pushq %rax\n"
        "    pushq %rdx\n"
        "    subq $32, %rsp\n"
        "    movdqu %xmm0, 0(%rsp)\n"
        "    movdqu %xmm1, 16(%rsp)\n"
        "    movq -8(%rbp), %rdi\n"
        "    movq -16(%rbp), %rsi\n"
        "    callq " SYM(bbcoop_context_lock_leave) "\n"
        "    movdqu 0(%rsp), %xmm0\n"
        "    movdqu 16(%rsp), %xmm1\n"
        "    addq $32, %rsp\n"
        "    popq %rdx\n"
        "    popq %rax\n"
        "    movq %rbp, %rsp\n"
        "    popq %rbp\n"
        "    retq\n");
extern char bbcoop_context_lock_common[];

/* jmp qword [rip+0]; .quad target */
static unsigned char *put_jump(unsigned char *at, uint64_t target) {
    static const unsigned char jmp[6] = {0xff, 0x25, 0, 0, 0, 0};
    memcpy(at, jmp, sizeof(jmp));
    memcpy(at + 6, &target, 8);
    return at + 14;
}

static void print_stats(void) {
    for (size_t i = 0; i < HOOK_COUNT; ++i) {
        const Hook *h = &hooks[i];
        fprintf(stderr, "Context lock: %s: %" PRIu64 " calls, %" PRIu64 " waits (%.1f ms in all), %" PRIu64 " timeouts\n",
                h->name, (uint64_t)atomic_load(&h->calls), (uint64_t)atomic_load(&h->waits),
                (double)atomic_load(&h->wait_ns) / 1e6, (uint64_t)atomic_load(&h->timeouts));
    }
}

/* Before the image is made read-only. A site that differs (another build, a patch) leaves both alone. */
const char *context_lock_install(unsigned char *image, uint64_t size);
const char *context_lock_install(unsigned char *image, uint64_t size) {
    const char *env = getenv("BB_CONTEXT_LOCK");
    if (env && env[0] == '0') return NULL;
    for (size_t i = 0; i < HOOK_COUNT; ++i) {
        const Hook *h = &hooks[i];
        if (h->offset + 14 > size || h->offset + h->prologue_size > size ||
            memcmp(image + h->offset, h->prologue, h->prologue_size)) {
            printf("Context lock: %s at 0x%" PRIx64 " differs from Bloodborne 1.09; not hooked\n", h->name, h->offset);
            return NULL;
        }
    }
    /* One stub page: per hook, mov r11, hook; jmp common, then the moved prologue and a jump back.
     * Absolute jumps: the page can be anywhere. */
    unsigned char *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) return "Context lock: no stub page";
    unsigned char *entries[HOOK_COUNT], *p = page;
    for (size_t i = 0; i < HOOK_COUNT; ++i) {
        Hook *h = &hooks[i];
        entries[i] = p;
        *p++ = 0x49; *p++ = 0xbb; /* movabs r11, imm64 */
        uint64_t hook_address = (uint64_t)(uintptr_t)h;
        memcpy(p, &hook_address, 8);
        p = put_jump(p + 8, (uint64_t)(uintptr_t)bbcoop_context_lock_common);
        p = (unsigned char *)(((uintptr_t)p + 15) & ~(uintptr_t)15);
        h->original = p;
        memcpy(p, h->prologue, h->prologue_size);
        p = put_jump(p + h->prologue_size, (uint64_t)(uintptr_t)(image + h->offset + h->prologue_size));
        p = (unsigned char *)(((uintptr_t)p + 15) & ~(uintptr_t)15);
    }
    if (mprotect(page, 4096, PROT_READ | PROT_EXEC)) {
        munmap(page, 4096);
        return "Context lock: stub page not executable";
    }
    for (size_t i = 0; i < HOOK_COUNT; ++i) {
        unsigned char *target = image + hooks[i].offset;
        put_jump(target, (uint64_t)(uintptr_t)entries[i]);
        memset(target + 14, 0xcc, hooks[i].prologue_size - 14); /* never reached */
    }
    printf("Context lock: the main thread's GPU setup and GXRenderThread's frame take turns (BB_CONTEXT_LOCK=0 off)\n");
    atexit(print_stats);
    return NULL;
}
