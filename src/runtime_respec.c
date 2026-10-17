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
 *                      and upgraded weapons. Only for throwaway profiles (tools/respec_test.sh).
 *   BB_TEST_WARP=<lamp> test cheat (bbcoop): once in game, to a lamp's arrival point: an area name
 *                      (warp_points below) or a respawn point ID, <map><block>29xx, as the map files
 *                      hold them (2412950: the first of m24_01). With BB_TEST_ENDGAME, instead of the
 *                      Dream. For testing an area (its frame rate) without playing to it. */
#define _GNU_SOURCE
#include "hooks.h"
#include "runtime.h"
#include "runtime_respec.h"
#include <dirent.h>
#include <ftw.h>
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
#define WORLD_CHR_MAN   0x0553E878 /* +0x60: the local player character */
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
#define GEM_GENERATE     0x01A88130 /* (out handle, instances, doping ref, modify ref, features, GemGenParam) */
#define HANDLE_RELEASE   0x01A8BC00
#define GEM_FEATURES     0x0555235C /* the chalice feature block the debug "add gem" passes */
#define ADD_ITEM_OR_STORAGE 0x01B5C8C0 /* what does not fit goes to the Messengers' storage */
/* The "WeaponUpgrade" menu (the Workshop's Fortify Weapon; the Doll opens it too). It lists every
 * weapon; an item I holds the inventory index at +0x48, the item key at +0x4C and its cost. Hooked for
 * refund mode. (FrpgMenuDlgEquipCustomShop at 0x1736140 is a dead leftover.) */
#define UPGRADE_LIST_FILTER_1 0x01C689E1 /* the tabs' "has a next level" filters: eax the id, esi the */
#define UPGRADE_LIST_FILTER_2 0x01C68B41 /* row to look up (id + 100); listed when that row exists */
#define UPGRADE_ITEM_COST   0x01BD2D73 /* rbx: I; the fresh cost at rbp-0x60 (echoes, insight, -, materials) */
#define UPGRADE_TARGET      0x01C5E54D /* r15: I; ebx: the target id (current + 100) */
#define UPGRADE_PANEL_COST  0x01C5E82B /* r15: I; the info panel's cost at rbp-0x150 (as at ITEM_COST) */
#define TEXT_REFERENCE   0x01AE8CD1 /* in the menu text lookup (rdi out, esi FMG, edx text id), after its pushes */
#define MENU_DIALOGS_FMG 204       /* MenuDialogs_SP */
#define FORTIFY_CONFIRM_TEXT 221000 /* "Use <?demandSoul?> Blood Echoes and ... to fortify weapon?" */
#define REFUND_CONFIRM_TEXT 221090 /* added by tools/respec_mod.py */
#define UPGRADE_APPLY       0x01B5DD88 /* r14: I; rbx: equipment; edx: the target, before ChangeItemId */

#define FLAG_RESPEC 72100900
#define FLAG_REFUND 72100901
#define FLAG_ENDGAME_GIVEN 72100902 /* BB_TEST_ENDGAME ran on this save */
#define DREAM_AREA  0x15       /* m21_xx: the Hunter's Dream */
#define SESSION_HOST 3
#define SESSION_CLIENT 6
#define MAX_ECHOES  999999999
#define MAX_INSIGHT 99
#define NO_WEAPON_ROW 0x63 /* what the game looks up for an invalid id: no such row */
#define BACKUPS_KEPT 20
#define CATEGORY_WEAPON 0x00000000u
#define CATEGORY_GOODS  0x40000000u

/* PlayerGameData. */
/* Vitality, Endurance, Strength, Skill, Bloodtinge, Arcane (0x50 is an unused Dark Souls stat). */
static const uint64_t pgd_stats[RESPEC_STAT_COUNT] = {0x40, 0x48, 0x58, 0x60, 0x68, 0x70};
#define PGD_INSIGHT_COPY 0x80
#define PGD_INSIGHT  0x84
#define PGD_LEVEL    0x90
#define PGD_ECHOES   0x94
#define PGD_HP       0x14 /* current, then max */
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
typedef void (ABI *GemGenerate)(uint32_t *out, uint64_t instances, const void *doping, const void *modify,
                                const void *features, int32_t gem_gen);
typedef void (ABI *HandleRelease)(uint32_t *handle);
typedef _Bool (ABI *AddItemOrStorage)(const uint32_t *key, int32_t count);
typedef void (ABI *ChangeItemQty)(uint64_t unused, uint32_t category, int32_t id, int32_t delta);
#define NATIVE(type, offset) ((type)(uintptr_t)(image_base + (offset)))

static uint64_t image_base;
static int respec_free, test_endgame, endgame_done, trace;
static int32_t test_warp; /* BB_TEST_WARP: 0 none */
static const struct { const char *name; int32_t point; } warp_points[] = {
    {"dream", 2102950}, {"cathedral-ward", 2402950}, {"central-yharnam", 2412950}, {"old-yharnam", 2422950},
};
/* Refund mode, from the Doll's writes of flag 72100901, which is never stored: a stored flag is saved
 * and would outlive a game quit with her menu open, into the Workshop's own Fortify menu. */
static int refund_requested;
static unsigned frames, dense; /* dense: frames of every-frame tracing left after a change */

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

static void trace_state(void);
static uint64_t inventory_entry(uint64_t inventory, uint32_t index);
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

/* The level-up commit adjusts current HP by the change in max HP, clamped at 0: with Vitality going
 * down that leaves 0 HP, and the character dies (dropping the refunded echoes). Fill HP to the new
 * maximum, in the player data and the character's live data (+0xF8 current, +0xFC max). */
static void fill_hp(uint64_t pgd) {
    uint64_t chr = read_u64(image_pointer(WORLD_CHR_MAN), 0x60), live = read_u64(read_u64(chr, 0x3B0), 0x20);
    int32_t max = read_s32(live, 0xFC);
    if (max > 0) write_s32(live, 0xF8, max);
    max = read_s32(pgd, PGD_HP + 4);
    if (max > 0) write_s32(pgd, PGD_HP, max);
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

static int remove_entry(const char *path, const struct stat *st, int type, struct FTW *ftw) {
    (void)st; (void)type; (void)ftw;
    return remove(path);
}
static int compare_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* Keeps the newest BACKUPS_KEPT backups (their names sort by time). */
static void prune_backups(const char *root) {
    char *names[512];
    size_t count = 0;
    DIR *d = opendir(root);
    if (!d) return;
    for (struct dirent *e; (e = readdir(d)) && count < 512;)
        if (e->d_name[0] != '.') names[count++] = strdup(e->d_name);
    closedir(d);
    qsort(names, count, sizeof(*names), compare_names);
    for (size_t i = 0; i < count; ++i) {
        if (i + BACKUPS_KEPT < count) {
            char path[1400];
            snprintf(path, sizeof(path), "%s/%s", root, names[i]);
            nftw(path, remove_entry, 8, FTW_DEPTH | FTW_PHYS);
        }
        free(names[i]);
    }
}

/* Copies every <user>/savedata/<id>/<title>/SPRJ0005 to a new <user>/respec-backups/<time>[-n]/
 * <id>-<title>/: the last save before the change. Returns the number of files copied, or -1. */
static int backup_saves(char *where, size_t where_size) {
    char stamp[32], root[1024];
    time_t now = time(NULL);
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", localtime(&now));
    snprintf(root, sizeof(root), "%s/respec-backups", runtime_file_user_dir());
    mkdir(root, 0755);
    prune_backups(root);
    int made = 0;
    for (int n = 0; n < 100 && !made; ++n) {
        if (n) snprintf(where, where_size, "%s/%s-%02d", root, stamp, n);
        else snprintf(where, where_size, "%s/%s", root, stamp);
        made = !mkdir(where, 0755);
        if (!made && errno != EEXIST) return -1;
    }
    if (!made) return -1;
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
                struct stat st;
                snprintf(from, sizeof(from), "%s/%s", save, f->d_name);
                if (stat(from, &st) || !S_ISREG(st.st_mode)) continue;
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
    if (trace) trace_state();
    apply_stats(start);
    fill_hp(pgd);
    if (trace) { trace_state(); dense = 900; }
    int64_t total = (int64_t)echoes + refund;
    if (total > MAX_ECHOES)
        printf("Respec: %lld echoes over the game's maximum are lost\n", (long long)(total - MAX_ECHOES));
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

/* The inventory index of a weapon by id, or -1 (the last one, so a just-given copy). */
static int32_t find_weapon(uint64_t pgd, int32_t id) {
    uint64_t inventory = pgd + PGD_INVENTORY;
    uint32_t total = read_u32(inventory, 0x24) + read_u32(inventory, 0x14);
    int32_t found = -1;
    for (uint32_t i = 0; i < total && i < 4096; ++i) {
        uint64_t entry = inventory_entry(inventory, i);
        if (read_u32(entry, 0) && read_u32(entry, 4) == (CATEGORY_WEAPON | (uint32_t)id)) found = (int32_t)i;
    }
    return found;
}

/* BB_TEST_ENDGAME: a late-game character for testing respec on a throwaway profile. */
static void make_endgame_character(uint64_t pgd) {
    static const RespecStats stats = {150, {50, 40, 25, 40, 30, 15}};
    static const struct { uint32_t category; int32_t id, count; } items[] = {
        {CATEGORY_WEAPON, 7001000, 1},  /* Saw Cleaver +10 */
        {CATEGORY_WEAPON, 7011000, 1},  /* Uncanny Saw Cleaver +10 */
        {CATEGORY_WEAPON, 14001000, 1}, /* Hunter Pistol +10: the vanilla gun slot */
        {CATEGORY_WEAPON, 23000500, 1}, /* Beasthunter Saif +5 (DLC) */
        /* The firearms that get a gem slot from the gun gems (tools/respec_mod.py). */
        {CATEGORY_WEAPON, 15000100, 1}, /* Cannon +1 */
        {CATEGORY_WEAPON, 18101000, 1}, /* Flamesprayer +10 */
        {CATEGORY_WEAPON, 18001000, 1}, /* Rosmarinus +10 (DLC) */
        {CATEGORY_WEAPON, 20001000, 1}, /* Hunter's Torch +10 */
        {CATEGORY_WEAPON, 34001000, 1}, /* Fist of Gratia +10 (DLC) */
        {CATEGORY_WEAPON, 35001000, 1}, /* Church Cannon +10 (DLC) */
        {CATEGORY_GOODS, RESPEC_GOODS_SHARD, 2},
        {CATEGORY_GOODS, 4103, 1},      /* Blood Gem Workshop Tool */
        {CATEGORY_GOODS, 900, 20},      /* Quicksilver Bullets */
        {CATEGORY_GOODS, 1000, 20},     /* Blood Vial */
        {CATEGORY_GOODS, 200, 1},       /* Beckoning Bell: the host rings it (bbcoop co-op tests) */
        {CATEGORY_GOODS, 205, 1},       /* Small Resonant Bell: the guest rings it */
    };
    char where[1200];
    printf("Respec: BB_TEST_ENDGAME: %d save files backed up in %s\n", backup_saves(where, sizeof(where)), where);
    apply_stats(&stats);
    fill_hp(pgd);
    write_s32(pgd, PGD_ECHOES, 500000);
    write_s32(pgd, PGD_INSIGHT, 20);
    write_s32(pgd, PGD_INSIGHT_COPY, 20);
    /* Weapons are given at +0 (the give path drops the level) and upgraded as the Workshop does. */
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); ++i) {
        int weapon = items[i].category == CATEGORY_WEAPON;
        int32_t id = weapon ? respec_weapon_base(items[i].id) : items[i].id;
        NATIVE(ChangeItemQty, CHANGE_ITEM_QTY)(0, items[i].category, id, items[i].count);
        int32_t index = weapon && id != items[i].id ? find_weapon(pgd, id) : -1;
        if (index >= 0) NATIVE(ChangeItemId, CHANGE_ITEM_ID)(pgd + PGD_EQUIPMENT, (uint32_t)index, items[i].id);
        else if (weapon && id != items[i].id) printf("Respec: BB_TEST_ENDGAME: weapon %d not found to upgrade\n", id);
    }
    /* A gem fitted in the Uncanny Saw Cleaver's first (Radial) slot, as the Blood Gem menu leaves it:
     * generated from GemGenParam 110001 (a Radial rough), the slot holding the only reference. */
    int32_t uncanny = find_weapon(pgd, 7011000);
    uint64_t instances = image_pointer(ITEM_INSTANCES);
    uint64_t weapon = uncanny >= 0 ? read_u64(instances, 8 + (uint64_t)(read_u32(inventory_entry(pgd + PGD_INVENTORY,
                                                                   (uint32_t)uncanny), 0) & 0xFFFF) * 8) : 0;
    if (weapon >= 0x10000 && (read_u32(weapon, 0x38) & 1) && !read_u32(weapon, 0x3C)) {
        static const struct { uint32_t id, pad; uint64_t row; } none = {UINT32_MAX, 0, 0};
        uint32_t gem = 0;
        NATIVE(GemGenerate, GEM_GENERATE)(&gem, instances, &none, &none, (const void *)(uintptr_t)(image_base + GEM_FEATURES),
                                          110001);
        if (gem) {
            NATIVE(HandleAssign, HANDLE_ASSIGN)(weapon + 0x3C, &gem);
            NATIVE(HandleRelease, HANDLE_RELEASE)(&gem);
        }
        printf("Respec: BB_TEST_ENDGAME: gem %#x fitted in the Uncanny Saw Cleaver +10\n", read_u32(weapon, 0x3C));
    } else printf("Respec: BB_TEST_ENDGAME: no open Radial slot for a gem\n");
    /* Loose gems to fit by hand, one GemGenParam row each with a fixed shape and fixed effects: Droplet
     * (slot type 0x8, the guns' shape), Radial, Triangle and Waning. */
    static const int32_t loose_gems[] = {
        110037, /* Droplet: Blood ATK UP +27% */
        110033, /* Droplet: Blood ATK UP +16.2% */
        134002, /* Droplet: Blood ATK UP +15.1% */
        136003, /* Droplet: Physical ATK UP +8.8%, Blood ATK UP +11.3% */
        124004, /* Droplet: Blood ATK UP +12.6% */
        110007, /* Radial: Physical ATK UP +22.5% */
        126004, /* Radial: ATK UP +15% */
        127001, /* Radial: SKL scaling +12 */
        110017, /* Triangle: Fire ATK UP +22.5% */
        136004, /* Triangle: Arcane ATK UP +4.1%, Add arcane ATK +47.3 */
        128000, /* Waning: Bolt ATK UP +16.5% */
        110027, /* Waning: HP continues to recover +4 */
    };
    int loose = 0;
    for (size_t i = 0; i < sizeof(loose_gems) / sizeof(loose_gems[0]); ++i) {
        static const struct { uint32_t id, pad; uint64_t row; } none = {UINT32_MAX, 0, 0};
        uint32_t gem = 0;
        NATIVE(GemGenerate, GEM_GENERATE)(&gem, instances, &none, &none, (const void *)(uintptr_t)(image_base + GEM_FEATURES),
                                          loose_gems[i]);
        if (!gem) continue;
        /* The inventory takes its own reference, as for a refunded gem. */
        if (NATIVE(AddHandle, ADD_HANDLE)(pgd + PGD_INVENTORY, &gem, 1, 0, 0) >= 0) ++loose;
        NATIVE(HandleRelease, HANDLE_RELEASE)(&gem);
    }
    printf("Respec: BB_TEST_ENDGAME: %d of %zu loose gems in the inventory\n", loose,
           sizeof(loose_gems) / sizeof(loose_gems[0]));
    /* Gun gems: the slotless guns get the pistol's slot from +1 when the mod is in (BB_GUN_GEMS). */
    static const int32_t guns[] = {15000100, 18101000, 18001000, 20001000, 34001000, 35001000};
    for (size_t i = 0; i < sizeof(guns) / sizeof(guns[0]); ++i) {
        int32_t index = find_weapon(pgd, guns[i]);
        uint64_t gun = index >= 0 ? read_u64(instances, 8 + (uint64_t)(read_u32(inventory_entry(pgd + PGD_INVENTORY,
                                                                  (uint32_t)index), 0) & 0xFFFF) * 8) : 0;
        uint32_t type = gun >= 0x10000 ? read_u32(gun, 0x38) : 0;
        printf("Respec: BB_TEST_ENDGAME: gun %d first gem slot %#x (%s)\n", guns[i], type,
               gun < 0x10000 ? "weapon not found" : type & 0x80000000u ? "closed" : "open");
    }
    /* The first Dream visit done (cutscene 9401 with the sickroom lamp 12417810, clinic death
     * 9400/9402, Doll respawn 9404), the Workshop open (9403) and the tutorials seen; the Doll wakes
     * when the Dream loads with 9401 and Insight >= 1. Then to the Dream's lamp arrival point. */
    static const int32_t flags[] = {9400, 9401, 9402, 9403, 9404, 12417810,
                                    6700, 6702, 6703, 6704, 6707, 6710, 6711, 6719};
    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i) set_flag(flags[i], 1);
    NATIVE(Warp, WARP_TO_RESPAWN)(test_warp ? test_warp : 2102950);
    printf("Respec: BB_TEST_ENDGAME: level %d, echoes %d, insight %d, %zu item kinds given; warping to %d\n",
           read_s32(pgd, PGD_LEVEL), read_s32(pgd, PGD_ECHOES), read_s32(pgd, PGD_INSIGHT),
           sizeof(items) / sizeof(items[0]), test_warp ? test_warp : 2102950);
}

static void trace_state(void) {
    uint64_t pgd = player_data();
    uint64_t chr = read_u64(image_pointer(WORLD_CHR_MAN), 0x60), live = read_u64(read_u64(chr, 0x3B0), 0x20);
    printf("Respec: trace frame %u: hp %d/%d/%d live %d/%d/%d sp %d/%d lua %#llx pgd %#llx map %#x level %d echoes %d insight %d origin %d "
           "flags %d%d%d session %d\n", frames, read_s32(pgd, 0x14), read_s32(pgd, 0x18), read_s32(pgd, 0x1C),
           read_s32(live, 0xF8), read_s32(live, 0xFC), read_s32(live, 0x100), read_s32(pgd, 0x30), read_s32(pgd, 0x34), (unsigned long long)image_pointer(LUA_EVENT_MAN),
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

/* Refund mode: the Doll's "Refund Weapon Upgrades" has the Fortify menu open (she writes 72100901
 * before opening it and again when it closes), in the Dream, with no co-op session. */
static int refund_mode(void) {
    return refund_requested && (current_packed_map() >> 24) == DREAM_AREA && !in_session();
}

/* Refunds the weapon at inventory index `index` (expected id `id`) before the game changes it to +0:
 * its gems to the inventory (or storage), its gem slots closed as on a new +0 weapon, every upgrade
 * material returned (what does not fit to storage). The echoes spent on upgrades are not. The menu
 * saves afterwards, as after an upgrade. Returns 1 when done. */
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
        if (NATIVE(AddHandle, ADD_HANDLE)(inventory, &gem, 1, 0, 0) < 0 &&
            NATIVE(AddHandle, ADD_HANDLE)(read_u64(pgd, PGD_STORAGE), &gem, 1, 0, 0) < 0) {
            /* Releasing the slot now would destroy the gem. The weapon stays as it is. */
            printf("Respec: refund stopped: no room for gem %#x in the inventory or storage (%d unfitted)\n", gem, gems);
            return 0;
        }
        static const uint32_t none = 0;
        NATIVE(HandleAssign, HANDLE_ASSIGN)(weapon + 0x3C + 16 * (uint64_t)s, &none);
        ++gems;
    }
    int32_t base = respec_weapon_base(id);
    /* The game's ChangeItemId runs next; it only ever opens slots, so close them first. */
    for (int s = 0; s < GEM_SLOTS; ++s) write_s32(weapon, 0x38 + 16 * (uint64_t)s, (int32_t)GEM_SLOT_CLOSED);
    RespecMaterial materials[RESPEC_MATERIAL_KINDS];
    int kinds = respec_upgrade_materials(level, materials);
    char given[160] = "";
    for (int k = 0; k < kinds; ++k) {
        uint32_t goods = CATEGORY_GOODS | (uint32_t)materials[k].goods;
        int added = NATIVE(AddItemOrStorage, ADD_ITEM_OR_STORAGE)(&goods, materials[k].count);
        size_t used = strlen(given);
        snprintf(given + used, sizeof(given) - used, "%s%d x%d%s", k ? ", " : "", materials[k].goods,
                 materials[k].count, added ? "" : " (NOT ADDED: inventory and storage full)");
    }
    printf("Respec: weapon refunded: %d (+%d) -> %d, %d gem(s) unfitted, materials %s; "
           "%d save files backed up in %s\n", id, level, base, gems, given, files, where);
    return 1;
}

static int item_level(uint64_t item, int32_t *id) {
    *id = (int32_t)(read_u32(item, 0x4C) & 0x0FFFFFFF);
    return (read_u32(item, 0x4C) & 0xF0000000u) == CATEGORY_WEAPON ? respec_weapon_level(*id) : -1;
}

/* Which weapons the list shows. The game lists a weapon when its next level's row exists; with the
 * Doll's menu open it lists those whose +0 row exists, upgraded ones (+10 too), and nothing else
 * (nothing at all when a refund is not allowed, in a co-op session). */
static void upgrade_list_filter(HookRegs *regs) {
    if (!refund_requested) return;
    int32_t id = (int32_t)(uint32_t)regs->rax;
    regs->rsi = refund_mode() && respec_weapon_level(id) >= 1 ? (uint32_t)respec_weapon_base(id) : NO_WEAPON_ROW;
}

/* An upgrade cost on the stack (echoes, Insight, -, then a material vector with begin at +0x18 and end
 * at +0x20): nothing. The game copies and frees the emptied vector as usual. */
static void blank_cost(uint64_t cost) {
    int32_t none[2] = {0, 0};
    uint64_t materials_begin = read_u64(cost + 0x18, 0);
    guest_access(cost, 0, none, sizeof(none), 1);
    guest_access(cost + 0x20, 0, &materials_begin, 8, 1);
}

/* The info panel's cost (computed for the preview, separately from the item's): nothing either. */
static void upgrade_panel_cost(HookRegs *regs) {
    if (refund_requested) blank_cost(regs->rbp - 0x150);
}

/* Menu text lookups: with the Doll's menu open, the Fortify confirm becomes "Refund this weapon's
 * upgrades?" instead of "Use 0 Blood Echoes and × 1 to fortify weapon?". */
static void text_reference(HookRegs *regs) {
    if ((uint32_t)regs->rdx == FORTIFY_CONFIRM_TEXT && (uint32_t)regs->rsi == MENU_DIALOGS_FMG && refund_requested)
        regs->rdx = REFUND_CONFIRM_TEXT;
}

/* Each item's cost, as the list, the affordability check and the payment see it: with the Doll's
 * menu open, nothing (no echoes, no Insight, an empty material list). */
static void upgrade_item_cost(HookRegs *regs) {
    if (trace) printf("Respec: trace upgrade_item_cost: item key %#x rbx %#llx rdx %#llx refund %d\n", read_u32(regs->rbx, 0x4C), (unsigned long long)regs->rbx, (unsigned long long)regs->rdx, refund_mode());
    if (refund_requested) blank_cost(regs->rbp - 0x60);
}

/* The target: +0 instead of the next level. The preview shows +0, and the menu's own execute
 * changes the weapon to it (ChangeItemId re-equips it when equipped). */
static void upgrade_target(HookRegs *regs) {
    if (trace) printf("Respec: trace upgrade_target: item key %#x rbx %#llx rdx %#llx refund %d\n", read_u32(regs->r15, 0x4C), (unsigned long long)regs->rbx, (unsigned long long)regs->rdx, refund_mode());
    if (!refund_mode()) return;
    int32_t id;
    if (item_level(regs->r15, &id) >= 1) regs->rbx = (uint32_t)respec_weapon_base(id);
}

/* Just before that ChangeItemId: gems out, materials back. With the Doll's menu open anything else,
 * or a failed refund, gives the game's ChangeItemId the weapon's own id, so nothing changes. */
static void upgrade_apply(HookRegs *regs) {
    if (trace) printf("Respec: trace upgrade_apply: item key %#x rbx %#llx rdx %#llx refund %d\n", read_u32(regs->r14, 0x4C), (unsigned long long)regs->rbx, (unsigned long long)regs->rdx, refund_mode());
    if (!refund_requested) return;
    int32_t id;
    int level = item_level(regs->r14, &id);
    uint64_t pgd = player_data();
    if (!refund_mode() || level < 1 || (int32_t)(uint32_t)regs->rdx != respec_weapon_base(id) || pgd < 0x10000 ||
        !refund_weapon(pgd, read_u32(regs->r14, 0x48), id))
        regs->rdx = (uint32_t)id;
}

static int in_game(uint64_t *pgd) {
    *pgd = player_data();
    return image_pointer(LUA_EVENT_MAN) >= 0x10000 && *pgd >= 0x10000 && current_packed_map() != 0;
}

/* Every update (the SOS status, which shadp2p also hooks: runs offline too). */
static void frame_update(HookRegs *regs) {
    (void)regs;
    uint64_t pgd;
    if (trace && (frames++ % 300 == 0 || (dense && dense-- % 10 == 0))) trace_state();
    /* A new load (the Lua event manager is recreated): the flags are saved, so a game quit with the
     * Doll's refund menu open would leave refund mode on for the Workshop's own Fortify menu. */
    static uint64_t loaded;
    uint64_t lua = image_pointer(LUA_EVENT_MAN);
    if (lua >= 0x10000 && lua != loaded && player_data() >= 0x10000) {
        loaded = lua;
        refund_requested = 0;
        if (get_flag(FLAG_REFUND)) set_flag(FLAG_REFUND, 0); /* saved by builds before it was never stored */
        if (get_flag(FLAG_RESPEC)) set_flag(FLAG_RESPEC, 0);
    }
    if (!(test_endgame || test_warp) || endgame_done || !in_game(&pgd)) return;
    endgame_done = 1;
    if (test_endgame && !get_flag(FLAG_ENDGAME_GIVEN)) {
        set_flag(FLAG_ENDGAME_GIVEN, 1);
        make_endgame_character(pgd);
    } else if (test_warp) {
        printf("Respec: BB_TEST_WARP: warping to %d\n", test_warp);
        NATIVE(Warp, WARP_TO_RESPAWN)(test_warp);
    }
}

/* BB_RESPEC_TRACE: the weapons, gems and upgrade materials in the inventory. */
static void dump_inventory(void) {
    uint64_t pgd = player_data(), inventory = pgd + PGD_INVENTORY;
    uint32_t total = read_u32(inventory, 0x24) + read_u32(inventory, 0x14);
    for (uint32_t i = 0; i < total && i < 2048; ++i) {
        uint64_t entry = inventory_entry(inventory, i);
        uint32_t handle = read_u32(entry, 0), key = read_u32(entry, 4);
        if (handle && (key >> 28 == 0 || key >> 28 == 8 || key == (CATEGORY_GOODS | 3000) || key == (CATEGORY_GOODS | 3010) ||
                       key == (CATEGORY_GOODS | 3020) || key == (CATEGORY_GOODS | 3030)))
            printf("Respec: trace inventory %u: handle %#x key %#x count %d\n", i, handle, key, read_s32(entry, 8));
    }
}

/* SetEventFlag(manager rdi, flag esi, on edx). The Doll's "Yes" sets 72100900: reset the attributes
 * here. Her refund menu writes 72100901: that is refund mode. Either way the original instructions
 * then write the flag as 0, so neither is ever stored (or saved). Other callers (about 85, some on
 * other threads) only pay for the compare. */
static void set_event_flag(HookRegs *regs) {
    uint32_t flag = (uint32_t)regs->rsi, on = (uint32_t)regs->rdx != 0;
    if (flag == FLAG_REFUND) {
        refund_requested = on;
        regs->rdx &= ~(uint64_t)UINT32_MAX;
        if (trace) {
            printf("Respec: trace refund mode %d\n", on);
            dump_inventory();
        }
        return;
    }
    if (flag != FLAG_RESPEC || !on) return;
    regs->rdx &= ~(uint64_t)UINT32_MAX;
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
    {"WeaponUpgrade.ListFilter1", UPGRADE_LIST_FILTER_1, 7, {0xC7,0x45,0xA0,0xFF,0xFF,0xFF,0xFF}, upgrade_list_filter},
    {"WeaponUpgrade.ListFilter2", UPGRADE_LIST_FILTER_2, 7, {0xC7,0x45,0xA0,0xFF,0xFF,0xFF,0xFF}, upgrade_list_filter},
    {"WeaponUpgrade.PanelCost", UPGRADE_PANEL_COST, 7, {0x48,0x8D,0xBD,0x18,0xFF,0xFF,0xFF}, upgrade_panel_cost},
    {"MenuText.Reference", TEXT_REFERENCE, 5, {0x41,0x89,0xD4,0x89,0xF3}, text_reference},
    {"WeaponUpgrade.ItemCost", UPGRADE_ITEM_COST, 8, {0x48,0x8D,0x7B,0x20,0x48,0x8D,0x75,0xB0}, upgrade_item_cost},
    {"WeaponUpgrade.Target", UPGRADE_TARGET, 10, {0xC7,0x85,0xF8,0xFE,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}, upgrade_target},
    {"WeaponUpgrade.Apply", UPGRADE_APPLY, 7, {0x41,0x8B,0x76,0x48,0x48,0x89,0xDF}, upgrade_apply},
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
    {"GemGenerate", GEM_GENERATE, 12, {0x55,0x48,0x89,0xE5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54}, NULL},
    {"HandleRelease", HANDLE_RELEASE, 12, {0x55,0x48,0x89,0xE5,0x41,0x56,0x53,0x48,0x89,0xFB,0x8B,0x33}, NULL},
    {"ChangePlayerItemQty", CHANGE_ITEM_QTY, 16,
     {0x55,0x48,0x89,0xE5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xEC}, NULL},
};

/* NULL on success (respec on, or off when any site's bytes do not match: then nothing is written and
 * the game runs without it, as any other image than Bloodborne 1.09 does), or a hook error. Call
 * after relocation, while the image is still writable. */
const char *respec_install(unsigned char *image, uint64_t size) {
    const char *env = getenv("BB_RESPEC");
    if (env && !strcmp(env, "0")) return NULL;
    size_t count = sizeof(sites) / sizeof(sites[0]), hooks = 0;
    for (size_t i = 0; i < count; ++i) {
        const Site *s = &sites[i];
        if (s->offset > size || s->length > size - s->offset || memcmp(image + s->offset, s->bytes, s->length)) {
            printf("Respec: off: site \"%s\" at %#llx does not match Bloodborne 1.09\n", s->name,
                   (unsigned long long)s->offset);
            return NULL;
        }
    }
    image_base = (uint64_t)(uintptr_t)image;
    respec_free = (env = getenv("BB_RESPEC_FREE")) && !strcmp(env, "1");
    test_endgame = (env = getenv("BB_TEST_ENDGAME")) && !strcmp(env, "1");
    if ((env = getenv("BB_TEST_WARP")) && *env) {
        for (size_t i = 0; i < sizeof(warp_points) / sizeof(warp_points[0]); ++i)
            if (!strcmp(env, warp_points[i].name)) test_warp = warp_points[i].point;
        if (!test_warp) test_warp = (int32_t)strtol(env, NULL, 10);
        if (test_warp % 10000 < 2900) {
            printf("Respec: BB_TEST_WARP=%s is no lamp (a name or <map><block>29xx); no warp\n", env);
            test_warp = 0;
        }
    }
    trace = (env = getenv("BB_RESPEC_TRACE")) && !strcmp(env, "1");
    const char *failure = hooks_init(image, size);
    for (size_t i = 0; !failure && i < count; ++i)
        if (sites[i].handler) {
            failure = hooks_install(sites[i].offset, sites[i].bytes, sites[i].length, sites[i].handler);
            ++hooks;
        }
    if (!failure) failure = hooks_seal();
    if (failure) return failure;
    printf("Respec: %zu hooks installed%s%s%s\n", hooks, respec_free ? ", free (BB_RESPEC_FREE)" : "",
           test_endgame ? ", BB_TEST_ENDGAME" : "", test_warp ? ", BB_TEST_WARP" : "");
    return NULL;
}
