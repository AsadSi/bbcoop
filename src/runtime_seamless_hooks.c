/* bbcoop: seamless co-op code hooks for 1.09 (EU/US, one image), from shadp2p's InstallSeamlessCoopPatches
 * and the functional part of its TraceEntry (src/core/bloodborne_re.cpp, GPL-2.0-or-later), without
 * the RE tracing. Offsets are image offsets. Every hook site and every game function the handlers
 * call is checked byte for byte before anything is written. The sites hold whole instructions
 * with no relative operands (shadp2p checked them with Zydis), as hooks_install requires. */
#define _GNU_SOURCE
#include "hooks.h"
#include "runtime.h"
#include "gpu/shim/bbcoop_placement.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Pointers to game objects, in the image. */
#define GLOBAL_STATE         0x05556678
#define LOCAL_STATE          0x0553E878
#define SUMMON_MANAGER_ROOT  0x0553B120
#define CURRENT_MAP_LIST     0x0553B148
#define MATCHING_STATE       0x05540290
#define SUMMON_SESSION_RULES 0x0553D6D0
/* Game functions. */
#define SET_FORCED_SUMMON_MAP         0x0156CF10
#define SET_FORCED_SUMMON_POSITION    0x0156CF20
#define SET_FORCED_SUMMON_ORIENTATION 0x0156CF40
#define SET_FORCED_SUMMON_WARP        0x0156CF60
#define SELECT_SUMMONED_PLACEMENT     0x01332BC0
#define SUMMONED_MAP_RELOAD           0x01336B90
#define STAGE_TRANSITION              0x013CDE30
#define SET_SUMMON_RELOAD_STATE       0x0178D9A0
#define USE_ITEM_NATIVE_APPLY         0x018F9720

#define HOST_EFFECT                9001
#define SMALL_RESONANT_BELL_GOODS  205
#define SMALL_RESONANT_BELL_EFFECT 9005
#define BELL_USE_ARGUMENT          17
#define RESUME_STABLE_OBSERVATIONS 20
#define RESUME_MAX_ATTEMPTS        2
#define RESUME_RETRY_NS            UINT64_C(15000000000)

typedef void (ABI *Setter)(const void *value);
typedef void (ABI *Action)(void);
typedef _Bool (ABI *Selector)(uint64_t context, int32_t warp_info_id);
typedef int32_t (ABI *Transition)(void);
typedef void (ABI *StateSetter)(uint64_t state);
typedef _Bool (ABI *UseItem)(uint64_t player, int32_t goods, int32_t argument);
#define NATIVE(type, offset) ((type)(uintptr_t)(image_base + (offset)))

_Static_assert(sizeof(BbcoopPlacement) == 0x18, "the game's summon placement descriptor");

static uint64_t image_base;
/* The map a guest handoff is travelling to: the deferred reload (SosStatus) and the reload state
 * at the stage descriptor each fire once for it. */
static uint32_t pending_reload_map, pending_reload_state_map;
/* After a pre-match warp: ring the Small Resonant Bell again once the guest has settled there. */
static __thread struct {
    uint32_t map, observations, attempts;
    int32_t area;
    int dispatched;
    uint64_t dispatched_at;
} resume;

/* The pointers followed here come from live game structures and may be stale. Each access arms
 * runtime_fault_recover, so a fault returns here through probe.c's handler instead of ending the
 * game (a write to a page the GPU side write-protects is resolved by that handler first and goes
 * through). runtime_memory_is_mapped would not do: it does not cover the image or other
 * host-owned memory the game points into, and a mapped page can still be unreadable. Bases below
 * 0x10000 are refused, as shadp2p does. Game functions are called with nothing armed. */
static int guest_access(uint64_t base, uint64_t offset, void *host, size_t size, int write) {
    if (base < 0x10000 || base + offset < base) return 0;
    void *guest = (void *)(uintptr_t)(base + offset);
    sigjmp_buf recover, *previous = runtime_fault_recover;
    if (sigsetjmp(recover, 0)) {
        runtime_fault_recover = previous;
        return 0;
    }
    runtime_fault_recover = &recover;
    __asm__ volatile("" ::: "memory");
    if (write) memcpy(guest, host, size);
    else memcpy(host, guest, size);
    __asm__ volatile("" ::: "memory");
    runtime_fault_recover = previous;
    return 1;
}
static int guest_read(uint64_t base, uint64_t offset, void *out, size_t size) { return guest_access(base, offset, out, size, 0); }
static int guest_write(uint64_t base, uint64_t offset, const void *value, size_t size) {
    return guest_access(base, offset, (void *)value, size, 1);
}
/* Unreadable reads as 0, like shadp2p's ReadValue. */
static uint64_t read_u64(uint64_t base, uint64_t offset) { uint64_t v = 0; guest_read(base, offset, &v, 8); return v; }
static uint32_t read_u32(uint64_t base, uint64_t offset) { uint32_t v = 0; guest_read(base, offset, &v, 4); return v; }
static int32_t read_s32(uint64_t base, uint64_t offset) { return (int32_t)read_u32(base, offset); }
static uint64_t image_pointer(uint64_t offset) { return read_u64(image_base, offset); }

static int usable_map(uint32_t map) { return map != 0 && map != UINT32_MAX; }
/* Atomically takes `map` out of `pending`; false when something else already did. */
static int claim(uint32_t *pending, uint32_t map) {
    return __atomic_compare_exchange_n(pending, &map, 0, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static uint64_t local_player(void) { return read_u64(image_pointer(LOCAL_STATE), 0x60); }

static int has_player_effect(uint64_t player, int32_t effect) {
    uint64_t node = read_u64(read_u64(player, 0x1C8), 0x08);
    for (int i = 0; node >= 0x10000 && i < 512; ++i) {
        uint64_t next = read_u64(node, 0x58);
        if (!(read_u32(node, 0x1C) & 0x800C0003u) && read_u64(node, 0x48) >= 0x10000 && read_s32(node, 0x40) == effect)
            return 1;
        if (next == node) break;
        node = next;
    }
    return 0;
}

static uint32_t current_packed_map(void) {
    uint64_t list = image_pointer(CURRENT_MAP_LIST), array = read_u64(list, 0x10);
    int32_t index = read_s32(list, 0x20);
    uint64_t entries = read_u64(array, 0x20);
    if (index < 0 || index >= read_s32(array, 0x18) || entries < 0x10000) return 0;
    return read_u32(entries + (uint64_t)index * 0xA0, 8);
}

/* The local character's map, area, position and heading. */
static int read_local_placement(BbcoopPlacement *placement) {
    if (!guest_read(image_pointer(SUMMON_MANAGER_ROOT), 0xA80, &placement->area, 4)) return 0;
    placement->packed_region = current_packed_map();
    if (!usable_map(placement->packed_region)) return 0;
    uint64_t world = read_u64(image_pointer(LOCAL_STATE), 0x60), character = read_u64(world, 0x58);
    uint64_t transform = read_u64(read_u64(read_u64(character, 0x08), 0x3B0), 0x68);
    float t[6]; /* +0x1D4 heading, +0x1E0 x, y, z */
    if (!guest_read(transform, 0x1D4, t, sizeof(t))) return 0;
    placement->heading = t[0];
    placement->x = t[3];
    placement->y = t[4];
    placement->z = t[5];
    return isfinite(t[0]) && isfinite(t[3]) && isfinite(t[4]) && isfinite(t[5]);
}

/* The global state is readable and no stage transition or forced warp is pending. */
static int stage_idle(uint64_t state) {
    uint8_t transition = 1, forced = 1;
    return guest_read(state, 0x08, &transition, 1) && guest_read(state, 0x1520, &forced, 1) && !transition && !forced;
}

/* The received host placement into the global state's summon placement fields. */
static int write_transported(uint64_t state, const BbcoopPlacement *p) {
    const float position[4] = {p->x, p->y, p->z, 1.0f}, orientation[4] = {0.0f, p->heading, 0.0f, 0.0f};
    return guest_write(state, 0x14C4, &p->packed_region, 4) && guest_write(state, 0x14D0, position, 16) &&
           guest_write(state, 0x14E0, orientation, 16);
}

static void set_forced_placement(uint64_t state) {
    NATIVE(Setter, SET_FORCED_SUMMON_POSITION)((const void *)(uintptr_t)(state + 0x14D0));
    NATIVE(Setter, SET_FORCED_SUMMON_ORIENTATION)((const void *)(uintptr_t)(state + 0x14E0));
    NATIVE(Setter, SET_FORCED_SUMMON_MAP)((const void *)(uintptr_t)(state + 0x14C4));
}

/* Arms the game's forced warp to the placement in `state`: true when it selected `map`. */
static int select_forced_warp(uint64_t state, uint32_t map) {
    set_forced_placement(state);
    NATIVE(Action, SET_FORCED_SUMMON_WARP)();
    return NATIVE(Selector, SELECT_SUMMONED_PLACEMENT)(0, -1) && read_u32(state, 0x0C) == map;
}

/* SummonBuild entry (host): a guest from another map gets the host's own placement in the prepared
 * summon descriptor (rsi + 4), and the host advertises it with its summon requests. */
static void summon_build_entry(HookRegs *regs) {
    BbcoopPlacement candidate, host;
    bbcoop_placement_set_host(NULL);
    if (!guest_read(regs->rsi, 4, &candidate, sizeof(candidate)) || !usable_map(candidate.packed_region)) return;
    if (!read_local_placement(&host) || host.packed_region == candidate.packed_region) return;
    if (guest_write(regs->rsi, 4, &host, sizeof(host))) bbcoop_placement_set_host(&host);
}

/* CSMultiPlayerIns cross-map handoff (summoned guest, object in rbx): when the host is on another
 * map, warp there through the game's forced-summon path and arm the reload for that map. */
static void guest_handoff(HookRegs *regs) {
    uint32_t role = 1;
    uint8_t ready = 0;
    uint64_t state = image_pointer(GLOBAL_STATE);
    if (!guest_read(regs->rbx, 0xE8, &role, 4) || role || !guest_read(state, 0x1648, &ready, 1) || !ready) return;
    BbcoopPlacement host;
    int consumed, transported = bbcoop_placement_received(&host, &consumed);
    uint32_t current = current_packed_map();
    uint32_t received = transported ? host.packed_region : read_u32(state, 0x14C4);
    uint32_t transport = transported ? host.packed_region : read_u32(state, 0x1644);
    if (!usable_map(current) || !usable_map(received) || received != transport || current == received) return;
    if ((transported && !write_transported(state, &host)) || !stage_idle(state)) return;
    float placement[8]; /* +0x14D0 position, +0x14E0 orientation */
    if (!guest_read(state, 0x14D0, placement, sizeof(placement))) return;
    for (int i = 0; i < 8; ++i)
        if (!isfinite(placement[i])) return;
    if (!select_forced_warp(state, received)) return;
    __atomic_store_n(&pending_reload_map, received, __ATOMIC_RELEASE);
    __atomic_store_n(&pending_reload_state_map, received, __ATOMIC_RELEASE);
    if (!NATIVE(Transition, STAGE_TRANSITION)()) {
        __atomic_store_n(&pending_reload_map, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&pending_reload_state_map, 0, __ATOMIC_RELEASE);
    }
}

/* Stage-warp descriptor finalized (descriptor in r9): once the handoff's target map has its
 * descriptor, start the summoned-map reload and put the session rules into reload state 3, then
 * re-apply the received host placement. */
static void stage_descriptor_finalize(HookRegs *regs) {
    uint32_t target = __atomic_load_n(&pending_reload_state_map, __ATOMIC_ACQUIRE), map = 0;
    if (!usable_map(target) || !guest_read(regs->r9, 0x08, &map, 4) || map != target) return;
    if (!claim(&pending_reload_state_map, target)) return;
    uint64_t rules = image_pointer(SUMMON_SESSION_RULES);
    int32_t reload_state;
    if (!guest_read(rules, 0x84, &reload_state, 4)) {
        __atomic_store_n(&pending_reload_state_map, target, __ATOMIC_RELEASE);
        return;
    }
    int cleared_deferred = claim(&pending_reload_map, target);
    NATIVE(Action, SUMMONED_MAP_RELOAD)();
    if (read_s32(rules, 0x84) != 3) NATIVE(StateSetter, SET_SUMMON_RELOAD_STATE)(rules);
    if (read_s32(rules, 0x84) != 3) {
        __atomic_store_n(&pending_reload_state_map, target, __ATOMIC_RELEASE);
        if (cleared_deferred) __atomic_store_n(&pending_reload_map, target, __ATOMIC_RELEASE);
        return;
    }
    BbcoopPlacement host;
    int consumed;
    uint64_t state = image_pointer(GLOBAL_STATE);
    if (bbcoop_placement_received(&host, &consumed) && host.packed_region == target && write_transported(state, &host))
        set_forced_placement(state);
}

/* Before matching (guest with an unconsumed host placement on another map): travel there. */
static void pre_match_guest_warp(void) {
    BbcoopPlacement host;
    int consumed;
    if (!bbcoop_placement_received(&host, &consumed) || consumed) return;
    uint32_t current = current_packed_map();
    if (!usable_map(current) || !usable_map(host.packed_region) || current == host.packed_region) return;
    uint64_t multi_play = image_pointer(MATCHING_STATE);
    if (multi_play >= 0x10000 && (read_s32(multi_play, 0x124) != 0 || read_u64(multi_play, 0x18) != 0)) return;
    uint64_t state = image_pointer(GLOBAL_STATE);
    if (!stage_idle(state) || __atomic_load_n(&pending_reload_map, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&pending_reload_state_map, __ATOMIC_ACQUIRE))
        return;
    if (!write_transported(state, &host) || !select_forced_warp(state, host.packed_region) ||
        !NATIVE(Transition, STAGE_TRANSITION)())
        return;
    bbcoop_placement_consume(&host);
    memset(&resume, 0, sizeof(resume));
    resume.map = host.packed_region;
    resume.area = host.area;
}

/* After a pre-match warp: once the guest has been settled at the host's map and area with matching
 * idle for 20 updates, use the Small Resonant Bell natively; retried once after 15 s. */
static void resume_responder(void) {
    if (!usable_map(resume.map)) return;
    BbcoopPlacement local;
    uint64_t multi_play = image_pointer(MATCHING_STATE), player = local_player(), controller = 1, probe;
    int32_t matching = 1;
    if (current_packed_map() != resume.map || !read_local_placement(&local) || local.area != resume.area ||
        !stage_idle(image_pointer(GLOBAL_STATE)) || !guest_read(multi_play, 0x124, &matching, 4) ||
        !guest_read(multi_play, 0x18, &controller, 8) || matching || controller || !guest_read(player, 0, &probe, 8)) {
        resume.observations = 0;
        return;
    }
    if (has_player_effect(player, SMALL_RESONANT_BELL_EFFECT)) {
        printf("Seamless co-op: responder resumed at map %#x area %d after %u native attempt(s)\n",
               resume.map, resume.area, resume.attempts);
        memset(&resume, 0, sizeof(resume));
        return;
    }
    if (resume.dispatched) {
        if (runtime_wait_clock() - resume.dispatched_at < RESUME_RETRY_NS || resume.attempts >= RESUME_MAX_ATTEMPTS) return;
        resume.dispatched = 0;
        resume.observations = 0;
    }
    if (++resume.observations < RESUME_STABLE_OBSERVATIONS) return;
    int result = NATIVE(UseItem, USE_ITEM_NATIVE_APPLY)(player, SMALL_RESONANT_BELL_GOODS, BELL_USE_ARGUMENT);
    ++resume.attempts;
    resume.dispatched = 1;
    resume.dispatched_at = runtime_wait_clock();
    printf("Seamless co-op: responder bell at map %#x area %d, attempt %u: %s\n", resume.map, resume.area,
           resume.attempts, result ? "dispatched" : "rejected");
}

/* SOS status update (every player, each update): refresh the advertised placement, then the
 * pre-match warp, the deferred reload and the responder resume. */
static void sos_status_update(HookRegs *regs) {
    (void)regs;
    BbcoopPlacement local;
    if (read_local_placement(&local)) bbcoop_placement_set_host(&local);
    pre_match_guest_warp();
    uint32_t target = __atomic_load_n(&pending_reload_map, __ATOMIC_ACQUIRE);
    if (usable_map(target) && current_packed_map() == target && stage_idle(image_pointer(GLOBAL_STATE)) &&
        claim(&pending_reload_map, target)) {
        __atomic_store_n(&pending_reload_state_map, 0, __ATOMIC_RELEASE);
        NATIVE(Action, SUMMONED_MAP_RELOAD)();
    }
    resume_responder();
}

/* Healing fountain availability (object in r13): the co-op host may use lanterns. */
static void healing_fountain(HookRegs *regs) {
    uint8_t blocked = 0;
    if (guest_read(regs->r13, 0x49, &blocked, 1) && blocked && has_player_effect(local_player(), HOST_EFFECT))
        guest_write(regs->r13, 0x49, &(uint8_t){0}, 1);
}

typedef struct {
    const char *name;
    uint64_t offset;
    unsigned length;
    unsigned char bytes[16];
    HookHandler handler; /* NULL: a game function the handlers call, checked only */
} Site;

static const Site sites[] = {
    {"SummonBuild.Entry", 0x01874710, 6, {0x55,0x48,0x89,0xE5,0x41,0x57}, summon_build_entry},
    {"CSMultiPlayerIns.CrossMapGuestHandoff", 0x01E4FF3A, 7, {0x48,0x8D,0x83,0xED,0x00,0x00,0x00}, guest_handoff},
    {"Warp.StageDescriptor.Finalize", 0x01944F23, 5, {0xB8,0x00,0x00,0x00,0xFF}, stage_descriptor_finalize},
    {"SosStatus.Update", 0x01872360, 6, {0x55,0x48,0x89,0xE5,0x41,0x57}, sos_status_update},
    {"HealingFountain.Availability.Host", 0x012F836E, 5, {0x41,0x80,0x7D,0x48,0x00}, healing_fountain},
    {"SetForcedSummonMap", SET_FORCED_SUMMON_MAP, 16,
     {0x48,0x8B,0x05,0x61,0x97,0xFE,0x03,0x8B,0x0F,0x89,0x88,0xF0,0x14,0x00,0x00,0xC3}, NULL},
    {"SetForcedSummonPosition", SET_FORCED_SUMMON_POSITION, 16,
     {0x48,0x8B,0x05,0x51,0x97,0xFE,0x03,0xC5,0xF8,0x28,0x07,0xC5,0xF8,0x29,0x80,0x00}, NULL},
    {"SetForcedSummonOrientation", SET_FORCED_SUMMON_ORIENTATION, 16,
     {0x48,0x8B,0x05,0x31,0x97,0xFE,0x03,0xC5,0xF8,0x28,0x07,0xC5,0xF8,0x29,0x80,0x10}, NULL},
    {"SetForcedSummonWarp", SET_FORCED_SUMMON_WARP, 15,
     {0x48,0x8B,0x05,0x11,0x97,0xFE,0x03,0xC6,0x80,0x20,0x15,0x00,0x00,0x01,0xC3}, NULL},
    {"SelectSummonedPlacement", SELECT_SUMMONED_PLACEMENT, 13,
     {0x55,0x48,0x89,0xE5,0x41,0x57,0x41,0x56,0x53,0x48,0x83,0xEC,0x18}, NULL},
    {"SummonedMapReload", SUMMONED_MAP_RELOAD, 15,
     {0x55,0x48,0x89,0xE5,0x48,0x8D,0x05,0x2D,0x45,0x20,0x04,0x48,0x83,0x38,0x00}, NULL},
    {"StageTransition", STAGE_TRANSITION, 14,
     {0x55,0x48,0x89,0xE5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x50}, NULL},
    {"SetSummonReloadState", SET_SUMMON_RELOAD_STATE, 11,
     {0xC7,0x87,0x84,0x00,0x00,0x00,0x03,0x00,0x00,0x00,0xC3}, NULL},
    {"UseItemNativeApply", USE_ITEM_NATIVE_APPLY, 16,
     {0x55,0x48,0x89,0xE5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xEC}, NULL},
};

/* Returns NULL on success, or a message naming the first site whose bytes do not match. Call
 * after relocation and seamless_apply, while the image is still writable. */
const char *seamless_hooks_install(unsigned char *image, uint64_t size) {
    static char error[160];
    size_t count = sizeof(sites) / sizeof(sites[0]), hooks = 0;
    if (size < GLOBAL_STATE + 8) return "seamless hooks: the image is too small for Bloodborne 1.09";
    for (size_t i = 0; i < count; ++i) {
        const Site *s = &sites[i];
        if (s->offset > size || s->length > size - s->offset || memcmp(image + s->offset, s->bytes, s->length)) {
            snprintf(error, sizeof(error), "seamless hook site \"%s\" at %#llx does not match Bloodborne 1.09",
                     s->name, (unsigned long long)s->offset);
            return error;
        }
    }
    image_base = (uint64_t)(uintptr_t)image;
    const char *failure = hooks_init(image, size);
    for (size_t i = 0; !failure && i < count; ++i)
        if (sites[i].handler) {
            failure = hooks_install(sites[i].offset, sites[i].bytes, sites[i].length, sites[i].handler);
            ++hooks;
        }
    if (!failure) failure = hooks_seal();
    if (failure) return failure;
    printf("Seamless co-op: %zu hooks installed\n", hooks);
    return NULL;
}
