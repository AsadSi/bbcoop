/* bbcoop: respec at the Doll (Bloodborne 1.09, EU/US, one image). The Doll's talk script
 * (tools/respec_mod.py) sets event flag 72100900 for "Reset Attributes" and 72100901 while
 * "Refund Weapon Upgrades" has the weapon list open; this file does the rest on the game thread,
 * from hooks on SetEventFlag (the talk script's c1_11 runs on the game thread) and on the per-update
 * SOS status (the session state, which also runs offline). Offsets are image offsets; every hook site and
 * every game function called is checked byte for byte first (docs/respec_re.md has the RE).
 *   BB_RESPEC=0        no respec hooks
 *   BB_RESPEC_FREE=1   the stat reset costs no Insight (testing)
 *   BB_RESPEC_TRACE=1  logs the player state the hooks see, every 5 s of frames
 *   BB_TEST_ENDGAME=1  test cheat: once in game, a high-level character with Echoes, Insight
 *                      and upgraded weapons. Only for throwaway profiles (tools/respec_test.sh). */
#define _GNU_SOURCE
#include "hooks.h"
#include "runtime.h"
#include "runtime_respec.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* Globals (pointers in the image). */
#define LUA_EVENT_MAN   0x0553B0C8
#define EVENT_FLAG_MAN  0x0553B100
#define GAME_DATA_MAN   0x0553B130
#define CURRENT_MAP_LIST 0x0553B148
#define SESSION_MANAGER 0x05540290
/* Game functions. */
#define FRAME_UPDATE     0x0187236D /* in SosStatus.Update, after its pushes (hooked) */
#define GET_EVENT_FLAG   0x013BCCF0
#define SET_EVENT_FLAG   0x013CFCC0
#define LEVEL_UP_COST    0x0157F050
#define APPLY_STATS      0x01C13D00 /* the level-up menu's commit task */
#define SAVE_REQUEST     0x01336DA0
#define CHANGE_ITEM_QTY  0x0131CB70
#define WARP_TO_RESPAWN  0x013CDF30 /* event instruction 2003[49] */
#define ITEM_INSTANCES   0x0553E990 /* weapon, armor and gem instances, by handle & 0xFFFF */
#define ADD_HANDLE       0x014DA3F0 /* inventory: put an existing instance (a gem) back */
#define HANDLE_ASSIGN    0x01A8BED0 /* reference-counted handle store */
#define CHANGE_ITEM_ID   0x014D1B30 /* the Workshop's in-place upgrade; re-equips if equipped */
#define ADD_ITEM_OR_STORAGE 0x01B5C8C0 /* what does not fit goes to the Messengers' storage */
/* The Workshop "Fortify Weapon" menu (FrpgMenuDlgEquipCustomShop); hooked for refund mode. */
#define FORTIFY_LIST_QUERY 0x01738599 /* r12d: the weapon id whose upgrades are listed */
#define FORTIFY_ENTRY_MATERIALS 0x01737EB9 /* eax: the entry's material set id */
#define FORTIFY_COST_RETURN 0x01739033 /* ebx: the echo cost being returned */
#define FORTIFY_EXECUTE 0x01736345 /* rdi: entries, rax: the chosen index */

#define FLAG_RESPEC 72100900
#define FLAG_REFUND 72100901
#define FLAG_ENDGAME_GIVEN 72100902 /* BB_TEST_ENDGAME ran on this save */
#define DREAM_AREA  0x15       /* m21_xx: the Hunter's Dream */
#define SESSION_HOST 3
#define SESSION_CLIENT 6
#define MAX_ECHOES  999999999
#define MAX_INSIGHT 99
#define CATEGORY_WEAPON 0x00000000u
#define CATEGORY_GOODS  0x40000000u

/* PlayerGameData. */
/* Vitality, Endurance, Strength, Skill, Bloodtinge, Arcane (0x50 is an unused Dark Souls stat). */
static const uint64_t pgd_stats[RESPEC_STAT_COUNT] = {0x40, 0x48, 0x58, 0x60, 0x68, 0x70};
#define PGD_INSIGHT_COPY 0x80
#define PGD_INSIGHT  0x84
#define PGD_LEVEL    0x90
#define PGD_ECHOES   0x94
#define PGD_ORIGIN   0xCE
#define PGD_EQUIPMENT 0x1D0
#define PGD_INVENTORY 0x328
#define PGD_STORAGE  0x5B0
#define GEM_SLOTS    5
#define GEM_SLOT_CLOSED 0x80000000u

typedef _Bool (ABI *GetFlag)(uint64_t man, int32_t flag);
typedef void (ABI *SetFlag)(uint64_t man, uint32_t flag, _Bool on);
typedef int32_t (ABI *LevelCost)(int32_t level);
typedef void (ABI *ApplyStats)(void *values);
typedef void (ABI *SaveRequest)(void);
typedef void (ABI *Warp)(int32_t respawn_point);
typedef int32_t (ABI *AddHandle)(uint64_t inventory, uint32_t *handle, int32_t count, int32_t sort, int32_t flag);
typedef void (ABI *HandleAssign)(uint64_t to, const uint32_t *from);
typedef void (ABI *ChangeItemId)(uint64_t equipment, uint32_t index, int32_t id);
typedef _Bool (ABI *AddItemOrStorage)(const uint32_t *key, int32_t count);
typedef void (ABI *ChangeItemQty)(uint64_t unused, uint32_t category, int32_t id, int32_t delta);
#define NATIVE(type, offset) ((type)(uintptr_t)(image_base + (offset)))

static uint64_t image_base;
static int respec_free, test_endgame, endgame_done, trace;
static unsigned frames;

/* Reads and writes into live game structures arm runtime_fault_recover, as in
 * runtime_seamless_hooks.c; game functions are called with nothing armed. */
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
static uint64_t read_u64(uint64_t base, uint64_t offset) { uint64_t v = 0; guest_access(base, offset, &v, 8, 0); return v; }
static int32_t read_s32(uint64_t base, uint64_t offset) { int32_t v = 0; guest_access(base, offset, &v, 4, 0); return v; }
static uint32_t read_u32(uint64_t base, uint64_t offset) { return (uint32_t)read_s32(base, offset); }
static int write_s32(uint64_t base, uint64_t offset, int32_t v) { return guest_access(base, offset, &v, 4, 1); }
static uint64_t image_pointer(uint64_t offset) { return read_u64(image_base, offset); }

static uint64_t player_data(void) { return read_u64(image_pointer(GAME_DATA_MAN), 0x08); }

static int get_flag(int32_t flag) {
    uint64_t man = image_pointer(EVENT_FLAG_MAN);
    return man >= 0x10000 && NATIVE(GetFlag, GET_EVENT_FLAG)(man, flag);
}
static void set_flag(int32_t flag, int on) {
    uint64_t man = image_pointer(EVENT_FLAG_MAN);
    if (man >= 0x10000) NATIVE(SetFlag, SET_EVENT_FLAG)(man, (uint32_t)flag, on != 0);
}

static uint32_t current_packed_map(void) {
    uint64_t list = image_pointer(CURRENT_MAP_LIST), array = read_u64(list, 0x10);
    int32_t index = read_s32(list, 0x20);
    uint64_t entries = read_u64(array, 0x20);
    if (index < 0 || index >= read_s32(array, 0x18) || entries < 0x10000) return 0;
    uint32_t map = 0;
    guest_access(entries + (uint64_t)index * 0xA0, 8, &map, 4, 0);
    return map;
}

static int in_session(void) {
    uint64_t session = image_pointer(SESSION_MANAGER);
    int32_t state = session >= 0x10000 ? read_s32(session, 0x124) : 0;
    return state == SESSION_HOST || state == SESSION_CLIENT;
}

static int read_stats(uint64_t pgd, RespecStats *s) {
    s->level = read_s32(pgd, PGD_LEVEL);
    for (int i = 0; i < RESPEC_STAT_COUNT; ++i) s->stat[i] = read_s32(pgd, pgd_stats[i]);
    return respec_stats_valid(s);
}

/* Sets level and stats through the level-up menu's own commit, which also recomputes HP, stamina
 * and the rest and refreshes the character. Its argument: int32 level at +8, the stats at +0xC. */
static void apply_stats(const RespecStats *s) {
    int32_t values[10] = {0};
    values[2] = s->level;
    for (int i = 0; i < RESPEC_STAT_COUNT; ++i) values[3 + i] = s->stat[i];
    NATIVE(ApplyStats, APPLY_STATS)(values);
}

static int32_t level_cost(int32_t level) { return NATIVE(LevelCost, LEVEL_UP_COST)(level); }

static int copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb"), *out = in ? fopen(to, "wb") : NULL;
    char buffer[65536];
    size_t n;
    int ok = in && out;
    while (ok && (n = fread(buffer, 1, sizeof(buffer), in)) > 0) ok = fwrite(buffer, 1, n, out) == n;
    if (in) fclose(in);
    if (out && fclose(out)) ok = 0;
    return ok;
}

/* Copies every <user>/savedata/<id>/<title>/SPRJ0005 to <user>/respec-backups/<time>/<id>-<title>/:
 * the last save before the change. Returns the number of files copied, or -1. */
static int backup_saves(char *where, size_t where_size) {
    char stamp[32], root[1024];
    time_t now = time(NULL);
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", localtime(&now));
    snprintf(root, sizeof(root), "%s/respec-backups", runtime_file_user_dir());
    mkdir(root, 0755);
    snprintf(where, where_size, "%s/%s", root, stamp);
    if (mkdir(where, 0755) && errno != EEXIST) return -1;
    char users[1024];
    snprintf(users, sizeof(users), "%s/savedata", runtime_file_user_dir());
    int files = 0;
    DIR *ud = opendir(users);
    if (!ud) return -1;
    for (struct dirent *u; (u = readdir(ud));) {
        if (u->d_name[0] == '.') continue;
        char titles[2048];
        snprintf(titles, sizeof(titles), "%s/%s", users, u->d_name);
        DIR *td = opendir(titles);
        if (!td) continue;
        for (struct dirent *t; (t = readdir(td));) {
            if (t->d_name[0] == '.' || strchr(t->d_name, '.')) continue;
            char save[3072], target[3072];
            snprintf(save, sizeof(save), "%s/%s/SPRJ0005", titles, t->d_name);
            snprintf(target, sizeof(target), "%s/%s-%s", where, u->d_name, t->d_name);
            DIR *sd = opendir(save);
            if (!sd) continue;
            mkdir(target, 0755);
            for (struct dirent *f; (f = readdir(sd));) {
                if (f->d_name[0] == '.') continue;
                char from[4096], to[4096];
                snprintf(from, sizeof(from), "%s/%s", save, f->d_name);
                snprintf(to, sizeof(to), "%s/%s", target, f->d_name);
                if (!copy_file(from, to)) { closedir(sd); closedir(td); closedir(ud); return -1; }
                ++files;
            }
            closedir(sd);
        }
        closedir(td);
    }
    closedir(ud);
    return files;
}

/* "Reset Attributes": back to the origin's level and stats; every echo spent on levels returned. */
static void reset_attributes(uint64_t pgd) {
    RespecStats now;
    unsigned char origin = 0xFF;
    guest_access(pgd, PGD_ORIGIN, &origin, 1, 0);
    const RespecStats *start = respec_origin(origin);
    int32_t insight = read_s32(pgd, PGD_INSIGHT), echoes = read_s32(pgd, PGD_ECHOES);
    if (!start || !read_stats(pgd, &now)) {
        printf("Respec: refused: unexpected player data (origin %u, level %d)\n", origin, now.level);
        return;
    }
    if (!memcmp(&now, start, sizeof(now))) {
        printf("Respec: nothing to reset (level %d is the origin's)\n", now.level);
        return;
    }
    if (!respec_free && insight < 1) {
        printf("Respec: refused: needs 1 Insight, has %d\n", insight);
        return;
    }
    int64_t refund = respec_echo_refund(start->level, now.level, level_cost);
    if (refund < 0) {
        printf("Respec: refused: the level cost function failed\n");
        return;
    }
    char where[1200];
    int files = backup_saves(where, sizeof(where));
    if (files < 0) {
        printf("Respec: refused: could not back up the saves to %s\n", where);
        return;
    }
    apply_stats(start);
    int64_t total = (int64_t)echoes + refund;
    write_s32(pgd, PGD_ECHOES, total > MAX_ECHOES ? MAX_ECHOES : (int32_t)total);
    if (!respec_free) {
        write_s32(pgd, PGD_INSIGHT, insight - 1);
        write_s32(pgd, PGD_INSIGHT_COPY, insight - 1);
    }
    RespecStats after;
    read_stats(pgd, &after);
    printf("Respec: attributes reset: level %d -> %d (vit %d end %d str %d skl %d bld %d arc %d), "
           "echoes %d + %lld -> %d, insight %d -> %d; %d save files backed up in %s\n",
           now.level, after.level, after.stat[0], after.stat[1], after.stat[2], after.stat[3], after.stat[4],
           after.stat[5], echoes, (long long)refund, read_s32(pgd, PGD_ECHOES), insight,
           read_s32(pgd, PGD_INSIGHT), files, where);
    NATIVE(SaveRequest, SAVE_REQUEST)();
}

/* BB_TEST_ENDGAME: a late-game character for testing respec on a throwaway profile. */
static void make_endgame_character(uint64_t pgd) {
    static const RespecStats stats = {150, {50, 40, 40, 40, 15, 15}};
    static const struct { uint32_t category; int32_t id, count; } items[] = {
        {CATEGORY_WEAPON, 7001000, 1},  /* Saw Cleaver +10 */
        {CATEGORY_WEAPON, 7011000, 1},  /* Uncanny Saw Cleaver +10 */
        {CATEGORY_WEAPON, 14001000, 1}, /* Hunter Pistol +10 */
        {CATEGORY_WEAPON, 23000500, 1}, /* Beasthunter Saif +5 (DLC) */
        {CATEGORY_GOODS, RESPEC_GOODS_SHARD, 2},
        {CATEGORY_GOODS, 4103, 1},      /* Blood Gem Workshop Tool */
    };
    apply_stats(&stats);
    write_s32(pgd, PGD_ECHOES, 500000);
    write_s32(pgd, PGD_INSIGHT, 20);
    write_s32(pgd, PGD_INSIGHT_COPY, 20);
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); ++i)
        NATIVE(ChangeItemQty, CHANGE_ITEM_QTY)(0, items[i].category, items[i].id, items[i].count);
    /* The first Dream visit done (cutscene 9401 with the sickroom lamp 12417810, clinic death
     * 9400/9402, Doll respawn 9404), the Workshop open (9403) and the tutorials seen; the Doll wakes
     * when the Dream loads with 9401 and Insight >= 1. Then to the Dream's lamp arrival point. */
    static const int32_t flags[] = {9400, 9401, 9402, 9403, 9404, 12417810,
                                    6700, 6702, 6703, 6704, 6707, 6710, 6711, 6719};
    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i) set_flag(flags[i], 1);
    NATIVE(Warp, WARP_TO_RESPAWN)(2102950);
    printf("Respec: BB_TEST_ENDGAME: level %d, echoes %d, insight %d, %zu item kinds given; warping to the Dream\n",
           read_s32(pgd, PGD_LEVEL), read_s32(pgd, PGD_ECHOES), read_s32(pgd, PGD_INSIGHT),
           sizeof(items) / sizeof(items[0]));
}

static void trace_state(void) {
    uint64_t pgd = player_data();
    printf("Respec: trace frame %u: lua %#llx pgd %#llx map %#x level %d echoes %d insight %d origin %d "
           "flags %d%d%d session %d\n", frames, (unsigned long long)image_pointer(LUA_EVENT_MAN),
           (unsigned long long)pgd, current_packed_map(), read_s32(pgd, PGD_LEVEL), read_s32(pgd, PGD_ECHOES),
           read_s32(pgd, PGD_INSIGHT), pgd >= 0x10000 ? (int)(read_s32(pgd, PGD_ORIGIN & ~3) >> 16 & 0xFF) : -1,
           get_flag(FLAG_RESPEC), get_flag(FLAG_REFUND), get_flag(FLAG_ENDGAME_GIVEN), in_session());
}

/* Inventory entry `index` (16 bytes: handle, category|id, count, sort): key items first, then the rest. */
static uint64_t inventory_entry(uint64_t inventory, uint32_t index) {
    uint32_t key_items = read_u32(inventory, 0x24);
    return index < key_items ? read_u64(inventory, 0x58) + (uint64_t)index * 16
                             : read_u64(inventory, 0x48) + (uint64_t)(index - key_items) * 16;
}

/* Refund mode: the Doll's "Refund Weapon Upgrades" has the Fortify menu open (flag 72100901, which
 * her talk script clears again), in the Dream, with no co-op session. */
static int refund_mode(void) {
    return get_flag(FLAG_REFUND) && (current_packed_map() >> 24) == DREAM_AREA && !in_session();
}

/* The weapon at inventory index `index` (expected id `id`) back to +0: its gems to the inventory
 * (or storage), its gem slots closed as on a new +0 weapon, every upgrade material returned. The
 * echoes spent on the upgrades are not. Returns 1 when done. */
static int refund_weapon(uint64_t pgd, uint32_t index, int32_t id) {
    uint64_t inventory = pgd + PGD_INVENTORY, entry = inventory_entry(inventory, index);
    uint32_t handle = read_u32(entry, 0), key = read_u32(entry, 4);
    int level = respec_weapon_level(id);
    uint64_t weapon = read_u64(image_pointer(ITEM_INSTANCES), 8 + (uint64_t)(handle & 0xFFFF) * 8);
    if (key != (uint32_t)id || !(handle & 0x800000) || level < 1 || read_s32(weapon, 0x0C) != id) {
        printf("Respec: refund refused: inventory %u holds %#x (handle %#x), expected %d\n", index, key, handle, id);
        return 0;
    }
    char where[1200];
    int files = backup_saves(where, sizeof(where));
    if (files < 0) {
        printf("Respec: refund refused: could not back up the saves to %s\n", where);
        return 0;
    }
    int gems = 0;
    for (int s = 0; s < GEM_SLOTS; ++s) {
        uint32_t gem = read_u32(weapon, 0x3C + 16 * (uint64_t)s);
        if (!gem) continue;
        if (NATIVE(AddHandle, ADD_HANDLE)(inventory, &gem, 1, 0, 0) < 0)
            NATIVE(AddHandle, ADD_HANDLE)(read_u64(pgd, PGD_STORAGE), &gem, 1, 0, 0);
        static const uint32_t none = 0;
        NATIVE(HandleAssign, HANDLE_ASSIGN)(weapon + 0x3C + 16 * (uint64_t)s, &none);
        ++gems;
    }
    int32_t base = respec_weapon_base(id);
    NATIVE(ChangeItemId, CHANGE_ITEM_ID)(pgd + PGD_EQUIPMENT, index, base);
    for (int s = 0; s < GEM_SLOTS; ++s) write_s32(weapon, 0x38 + 16 * (uint64_t)s, (int32_t)GEM_SLOT_CLOSED);
    RespecMaterial materials[RESPEC_MATERIAL_KINDS];
    int kinds = respec_upgrade_materials(level, materials);
    char given[160] = "";
    for (int k = 0; k < kinds; ++k) {
        uint32_t goods = CATEGORY_GOODS | (uint32_t)materials[k].goods;
        NATIVE(AddItemOrStorage, ADD_ITEM_OR_STORAGE)(&goods, materials[k].count);
        size_t used = strlen(given);
        snprintf(given + used, sizeof(given) - used, "%s%d x%d", k ? ", " : "", materials[k].goods, materials[k].count);
    }
    printf("Respec: weapon refunded: %d (+%d) -> %d (now %d), %d gem(s) unfitted, materials %s; "
           "%d save files backed up in %s\n", id, level, base, read_s32(weapon, 0x0C), gems, given, files, where);
    NATIVE(SaveRequest, SAVE_REQUEST)();
    return 1;
}

/* The menu lists, for each weapon, the rows that upgrade from it. In refund mode it is asked for
 * the rows that upgrade from one level below, which is the weapon itself (+10 too); +0 weapons
 * are asked for -1, which lists nothing. */
static void fortify_list_query(HookRegs *regs) {
    if (trace) printf("Respec: trace fortify_list_query: rdi %#llx rax %#llx rbx %#llx r12 %#llx refund %d\n", (unsigned long long)regs->rdi, (unsigned long long)regs->rax, (unsigned long long)regs->rbx, (unsigned long long)regs->r12, refund_mode());
    if (!refund_mode()) return;
    int32_t id = (int32_t)(uint32_t)regs->r12;
    regs->r12 = respec_weapon_level(id) >= 1 ? (uint32_t)(id - 100) : UINT32_MAX;
}
/* Entries carry material set -1 (none): nothing shown, nothing checked, nothing taken. */
static void fortify_entry_materials(HookRegs *regs) {
    if (trace) printf("Respec: trace fortify_entry_materials: rdi %#llx rax %#llx rbx %#llx r12 %#llx refund %d\n", (unsigned long long)regs->rdi, (unsigned long long)regs->rax, (unsigned long long)regs->rbx, (unsigned long long)regs->r12, refund_mode());
    if (refund_mode()) regs->rax = UINT32_MAX;
}
/* No echo cost in the list, the checks or the execute. */
static void fortify_cost_return(HookRegs *regs) {
    if (trace) printf("Respec: trace fortify_cost_return: rdi %#llx rax %#llx rbx %#llx r12 %#llx refund %d\n", (unsigned long long)regs->rdi, (unsigned long long)regs->rax, (unsigned long long)regs->rbx, (unsigned long long)regs->r12, refund_mode());
    if (refund_mode()) regs->rbx &= ~(uint64_t)UINT32_MAX;
}
/* The confirmed choice: refund it, then clear the entry's "owned" byte so the game skips its
 * ChangeItemId; the rest of its execute (no cost, no materials, refresh, list rebuild) still runs. */
static void fortify_execute(HookRegs *regs) {
    if (trace) printf("Respec: trace fortify_execute: rdi %#llx rax %#llx rbx %#llx r12 %#llx refund %d\n", (unsigned long long)regs->rdi, (unsigned long long)regs->rax, (unsigned long long)regs->rbx, (unsigned long long)regs->r12, refund_mode());
    if (!refund_mode()) return;
    uint64_t entry = regs->rdi + regs->rax * 16, pgd = player_data();
    uint32_t index = read_u32(entry, 4);
    int32_t id = read_s32(entry, 0x0C);
    if (pgd >= 0x10000) refund_weapon(pgd, index, id);
    unsigned char skip = 0;
    guest_access(entry, 0, &skip, 1, 1);
}

static int in_game(uint64_t *pgd) {
    *pgd = player_data();
    return image_pointer(LUA_EVENT_MAN) >= 0x10000 && *pgd >= 0x10000 && current_packed_map() != 0;
}

/* Every update (the SOS status, which shadp2p also hooks: runs offline too). */
static void frame_update(HookRegs *regs) {
    (void)regs;
    uint64_t pgd;
    if (trace && frames++ % 300 == 0) trace_state();
    if (!test_endgame || endgame_done || !in_game(&pgd)) return;
    endgame_done = 1;
    if (!get_flag(FLAG_ENDGAME_GIVEN)) {
        set_flag(FLAG_ENDGAME_GIVEN, 1);
        make_endgame_character(pgd);
    }
}

/* SetEventFlag(manager rdi, flag esi, on dl). The Doll's "Yes" sets 72100900: reset the attributes
 * here, then let the original instructions write the flag as 0, so it never stays set. Other
 * callers (about 85, some on other threads) only pay for the compare. */
static void set_event_flag(HookRegs *regs) {
    if ((uint32_t)regs->rsi != FLAG_RESPEC || !(regs->rdx & 0xFF)) return;
    regs->rdx &= ~(uint64_t)0xFF;
    uint64_t pgd;
    if (!in_game(&pgd)) printf("Respec: refused: not in game\n");
    else if ((current_packed_map() >> 24) != DREAM_AREA) printf("Respec: refused: not in the Hunter's Dream\n");
    else if (in_session()) printf("Respec: refused: a co-op session is active\n");
    else reset_attributes(pgd);
}

typedef struct {
    const char *name;
    uint64_t offset;
    unsigned length;
    unsigned char bytes[16];
    HookHandler handler; /* NULL: a game function called, checked only */
} Site;

static const Site sites[] = {
    {"SosStatus.Update+0xD", FRAME_UPDATE, 7, {0x48,0x81,0xEC,0xE8,0x06,0x00,0x00}, frame_update},
    {"SetEventFlag", SET_EVENT_FLAG, 6, {0x41,0x89,0xD0,0x8B,0x4F,0x1C}, set_event_flag},
    {"Fortify.ListQuery", FORTIFY_LIST_QUERY, 7, {0x48,0xC7,0x00,0x00,0x00,0x00,0x00}, fortify_list_query},
    {"Fortify.EntryMaterials", FORTIFY_ENTRY_MATERIALS, 7, {0x48,0x8B,0x8D,0x68,0xFE,0xFF,0xFF},
     fortify_entry_materials},
    {"Fortify.CostReturn", FORTIFY_COST_RETURN, 9, {0x89,0xD8,0x48,0x81,0xC4,0xB0,0x00,0x00,0x00},
     fortify_cost_return},
    {"Fortify.Execute", FORTIFY_EXECUTE, 6, {0x89,0xB5,0x20,0xFE,0xFF,0xFF}, fortify_execute},
    {"GetEventFlag", GET_EVENT_FLAG, 16,
     {0x85,0xF6,0x78,0x67,0x8B,0x4F,0x1C,0x31,0xD2,0x89,0xF0,0xF7,0xF1,0x41,0x89,0xC0}, NULL},
    {"SetEventFlag.Body", SET_EVENT_FLAG + 6, 10, {0x31,0xD2,0x89,0xF0,0xF7,0xF1,0x41,0x89,0xC1,0x44}, NULL},
    {"GetLevelUpCost", LEVEL_UP_COST, 16,
     {0x55,0x48,0x89,0xE5,0x41,0x56,0x53,0x48,0x83,0xEC,0x20,0x89,0xFB,0x4C,0x8B,0x35}, NULL},
    {"LevelUpCommit", APPLY_STATS, 16,
     {0x55,0x48,0x89,0xE5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xEC}, NULL},
    {"SaveRequest", SAVE_REQUEST, 16,
     {0x55,0x48,0x89,0xE5,0x48,0x8D,0x05,0x1D,0x43,0x20,0x04,0x48,0x83,0x38,0x00,0x75}, NULL},
    {"WarpToRespawnPoint", WARP_TO_RESPAWN, 16,
     {0x55,0x48,0x89,0xE5,0x41,0x57,0x41,0x56,0x53,0x48,0x83,0xEC,0x38,0x89,0xFB,0x4C}, NULL},
    {"AddHandle", ADD_HANDLE, 16,
     {0x55,0x48,0x89,0xE5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xEC}, NULL},
    {"HandleAssign", HANDLE_ASSIGN, 16,
     {0x55,0x48,0x89,0xE5,0x41,0x57,0x41,0x56,0x53,0x50,0x49,0x89,0xF6,0x48,0x89,0xFB}, NULL},
    {"ChangeItemId", CHANGE_ITEM_ID, 16,
     {0x55,0x48,0x89,0xE5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xEC}, NULL},
    {"AddItemOrStorage", ADD_ITEM_OR_STORAGE, 16,
     {0x55,0x48,0x89,0xE5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x48,0x83,0xEC,0x30,0x89}, NULL},
    {"ChangePlayerItemQty", CHANGE_ITEM_QTY, 16,
     {0x55,0x48,0x89,0xE5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xEC}, NULL},
};

/* NULL on success, or a message naming the first site whose bytes do not match. Call after
 * relocation, while the image is still writable. */
const char *respec_install(unsigned char *image, uint64_t size) {
    static char error[160];
    const char *env = getenv("BB_RESPEC");
    if (env && !strcmp(env, "0")) return NULL;
    size_t count = sizeof(sites) / sizeof(sites[0]), hooks = 0;
    for (size_t i = 0; i < count; ++i) {
        const Site *s = &sites[i];
        if (s->offset > size || s->length > size - s->offset || memcmp(image + s->offset, s->bytes, s->length)) {
            snprintf(error, sizeof(error), "respec site \"%s\" at %#llx does not match Bloodborne 1.09", s->name,
                     (unsigned long long)s->offset);
            return error;
        }
    }
    image_base = (uint64_t)(uintptr_t)image;
    respec_free = (env = getenv("BB_RESPEC_FREE")) && !strcmp(env, "1");
    test_endgame = (env = getenv("BB_TEST_ENDGAME")) && !strcmp(env, "1");
    trace = (env = getenv("BB_RESPEC_TRACE")) && !strcmp(env, "1");
    const char *failure = hooks_init(image, size);
    for (size_t i = 0; !failure && i < count; ++i)
        if (sites[i].handler) {
            failure = hooks_install(sites[i].offset, sites[i].bytes, sites[i].length, sites[i].handler);
            ++hooks;
        }
    if (!failure) failure = hooks_seal();
    if (failure) return failure;
    printf("Respec: %zu hooks installed%s%s\n", hooks, respec_free ? ", free (BB_RESPEC_FREE)" : "",
           test_endgame ? ", BB_TEST_ENDGAME" : "");
    return NULL;
}
