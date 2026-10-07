/* bbcoop: runtime_seamless_hooks.c on a synthetic image (x86-64 Linux). Covers the all-or-nothing
 * site check, installing the five hooks, and the two handlers that call no game function: the host
 * placement rewrite and the host's lantern availability, including bad game pointers. The handlers
 * that call game functions (guest handoff, reload state, SOS status) need the real game. */
#define _GNU_SOURCE
#include "gpu/shim/bbcoop_placement.h"
#include <assert.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

const char *seamless_hooks_install(unsigned char *image, uint64_t size);

#define SIZE 0x05600000
__thread sigjmp_buf *runtime_fault_recover;
static unsigned char *image, *heap;

/* The bridge (gpu/shim/bbcoop_placement.cpp): records what the host would advertise. */
static BbcoopPlacement advertised;
static int advertising;
void bbcoop_placement_set_host(const BbcoopPlacement *placement) {
    advertising = placement != NULL;
    if (placement) advertised = *placement;
}
int bbcoop_placement_received(BbcoopPlacement *out, int *consumed) { (void)out; (void)consumed; return 0; }
void bbcoop_placement_consume(const BbcoopPlacement *placement) { (void)placement; }

/* probe.c's recovery for speculative guest reads. */
static void fault(int sig, siginfo_t *info, void *context) {
    (void)info; (void)context;
    if (!runtime_fault_recover) abort();
    sigjmp_buf *recover = runtime_fault_recover;
    runtime_fault_recover = NULL;
    sigset_t unblock;
    sigemptyset(&unblock);
    sigaddset(&unblock, sig);
    sigprocmask(SIG_UNBLOCK, &unblock, NULL);
    siglongjmp(*recover, 1);
}

static void put(uint64_t offset, const void *bytes, size_t length) { memcpy(image + offset, bytes, length); }
static uint64_t at(size_t offset) { return (uint64_t)(uintptr_t)(heap + offset); }
static void ptr(uint64_t base, size_t offset, uint64_t value) { memcpy((void *)(uintptr_t)(base + offset), &value, 8); }
static void u32(uint64_t base, size_t offset, uint32_t value) { memcpy((void *)(uintptr_t)(base + offset), &value, 4); }
static void f32(uint64_t base, size_t offset, float value) { memcpy((void *)(uintptr_t)(base + offset), &value, 4); }

/* The 01.09 bytes at every hook site and checked game function. */
static void original(void) {
    put(0x01874710, "\x55\x48\x89\xE5\x41\x57", 6);
    put(0x01E4FF3A, "\x48\x8D\x83\xED\x00\x00\x00", 7);
    put(0x01944F23, "\xB8\x00\x00\x00\xFF", 5);
    put(0x01872360, "\x55\x48\x89\xE5\x41\x57", 6);
    put(0x012F836E, "\x41\x80\x7D\x48\x00", 5);
    put(0x0156CF10, "\x48\x8B\x05\x61\x97\xFE\x03\x8B\x0F\x89\x88\xF0\x14\x00\x00\xC3", 16);
    put(0x0156CF20, "\x48\x8B\x05\x51\x97\xFE\x03\xC5\xF8\x28\x07\xC5\xF8\x29\x80\x00", 16);
    put(0x0156CF40, "\x48\x8B\x05\x31\x97\xFE\x03\xC5\xF8\x28\x07\xC5\xF8\x29\x80\x10", 16);
    put(0x0156CF60, "\x48\x8B\x05\x11\x97\xFE\x03\xC6\x80\x20\x15\x00\x00\x01\xC3", 15);
    put(0x01332BC0, "\x55\x48\x89\xE5\x41\x57\x41\x56\x53\x48\x83\xEC\x18", 13);
    put(0x01336B90, "\x55\x48\x89\xE5\x48\x8D\x05\x2D\x45\x20\x04\x48\x83\x38\x00", 15);
    put(0x013CDE30, "\x55\x48\x89\xE5\x41\x57\x41\x56\x41\x55\x41\x54\x53\x50", 14);
    put(0x0178D9A0, "\xC7\x87\x84\x00\x00\x00\x03\x00\x00\x00\xC3", 11);
    put(0x018F9720, "\x55\x48\x89\xE5\x41\x57\x41\x56\x41\x55\x41\x54\x53\x48\x83\xEC", 16);
}

int main(void) {
    image = mmap(NULL, SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    heap = calloc(1, 0x10000);
    unsigned char *copy = malloc(SIZE);
    assert(image != MAP_FAILED && heap && copy);
    struct sigaction action = {.sa_sigaction = fault, .sa_flags = SA_SIGINFO};
    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGBUS, &action, NULL);

    /* A blank (wrong) build: refused, nothing written. */
    assert(seamless_hooks_install(image, SIZE) != NULL);
    for (size_t i = 0; i < SIZE; ++i) assert(image[i] == 0);

    /* Right hook sites but one changed game function: refused, still nothing written. */
    original();
    image[0x013CDE30 + 13] = 0x90;
    memcpy(copy, image, SIZE);
    const char *error = seamless_hooks_install(image, SIZE);
    assert(error && strstr(error, "StageTransition"));
    assert(!memcmp(copy, image, SIZE));
    image[0x013CDE30 + 13] = 0x50;

    /* An image too small for the game's globals: refused. */
    assert(seamless_hooks_install(image, 0x05000000) != NULL);

    /* Code after two sites, so they can be called: SummonBuild's prologue ends in pop r15; pop rbp;
     * ret, the fountain's compare in ret. A thunk at 0x1000 calls the fountain with r13 = rdi. */
    put(0x01874716, "\x41\x5F\x5D\xC3", 4);
    put(0x012F8373, "\xC3", 1);
    int32_t rel = 0x012F836E - (0x1000 + 10);
    put(0x1000, "\x41\x55\x49\x89\xFD\xE8", 6);
    put(0x1006, &rel, 4);
    put(0x100A, "\x41\x5D\xC3", 3);

    /* The real bytes: every hook installed (a jmp at each site), and a second install refused. */
    assert(seamless_hooks_install(image, SIZE) == NULL);
    const uint64_t hooked[] = {0x01874710, 0x01E4FF3A, 0x01944F23, 0x01872360, 0x012F836E};
    for (size_t i = 0; i < 5; ++i) assert(image[hooked[i]] == 0xE9);
    assert(seamless_hooks_install(image, SIZE) != NULL);

    void (*summon_build)(uint64_t, uint64_t) = (void (*)(uint64_t, uint64_t))(image + 0x01874710);
    void (*fountain)(uint64_t) = (void (*)(uint64_t))(image + 0x1000);
    void *gone = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    munmap(gone, 4096);
    const uint64_t unmapped = (uint64_t)(uintptr_t)gone;

    /* The game's local player (0x0553E878 -> +0x60) with effect 9001 (co-op host) in its list. */
    const uint64_t local = at(0), player = at(0x1000), effects = at(0x2000), node = at(0x3000), fountain_object = at(0x4000);
    ptr((uint64_t)(uintptr_t)image, 0x0553E878, local);
    ptr(local, 0x60, player);
    ptr(player, 0x1C8, effects);
    ptr(effects, 0x08, node);
    ptr(node, 0x58, node);
    ptr(node, 0x48, node);
    u32(node, 0x40, 9001);

    /* The host may use a blocked lantern; anyone else may not. */
    heap[0x4049] = 1;
    fountain(fountain_object);
    assert(heap[0x4049] == 0);
    heap[0x4049] = 1;
    u32(node, 0x40, 9005);
    fountain(fountain_object);
    assert(heap[0x4049] == 1);
    /* A bad pointer in the player's chain: the handler gives up, the game carries on. */
    u32(node, 0x40, 9001);
    ptr(player, 0x1C8, unmapped);
    fountain(fountain_object);
    assert(heap[0x4049] == 1);
    ptr(player, 0x1C8, effects);

    /* The host's placement: map from the current map list, area from the summon manager, position
     * and heading from the character transform. */
    const uint64_t list = at(0x5000), maps = at(0x6000), entries = at(0x7000), root = at(0x8000);
    const uint64_t character = at(0x9000), character_state = at(0xA000), model = at(0xB000), transform = at(0xC000);
    const uint64_t prepared = at(0xE000);
    ptr((uint64_t)(uintptr_t)image, 0x0553B148, list);
    ptr(list, 0x10, maps);
    u32(list, 0x20, 1);
    u32(maps, 0x18, 2);
    ptr(maps, 0x20, entries);
    u32(entries, 0xA0 + 8, 0x15000000);
    ptr((uint64_t)(uintptr_t)image, 0x0553B120, root);
    u32(root, 0xA80, 7);
    ptr(player, 0x58, character);
    ptr(character, 0x08, character_state);
    ptr(character_state, 0x3B0, model);
    ptr(model, 0x68, transform);
    f32(transform, 0x1D4, 1.5f);
    f32(transform, 0x1E0, 10.0f);
    f32(transform, 0x1E4, 20.0f);
    f32(transform, 0x1E8, 30.0f);

    /* A guest from another map: the prepared descriptor gets the host's placement, which is also
     * what the host advertises. */
    const BbcoopPlacement host = {0x15000000, 10.0f, 20.0f, 30.0f, 1.5f, 7};
    const BbcoopPlacement guest = {0x18010000, 1.0f, 2.0f, 3.0f, 0.5f, 4};
    memcpy(heap + 0xE004, &guest, sizeof(guest));
    summon_build(0, prepared);
    assert(!memcmp(heap + 0xE004, &host, sizeof(host)));
    assert(advertising && !memcmp(&advertised, &host, sizeof(host)));
    /* Same map: left alone, nothing advertised. */
    summon_build(0, prepared);
    assert(!memcmp(heap + 0xE004, &host, sizeof(host)) && !advertising);
    /* No readable transform: left alone. */
    memcpy(heap + 0xE004, &guest, sizeof(guest));
    ptr(model, 0x68, unmapped);
    summon_build(0, prepared);
    assert(!memcmp(heap + 0xE004, &guest, sizeof(guest)) && !advertising);
    /* A bad descriptor pointer. */
    summon_build(0, unmapped);
    assert(!advertising);
    return 0;
}
