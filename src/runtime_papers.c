/* bbcoop: Fire Paper, Bolt Paper and Empty Phantasm Shell scale with the attack (1.09, EU/US, one image).
 * In vanilla each adds a flat +80 fire, bolt or arcane to every hit of the right-hand weapon, after the
 * attack's multiplier (motion value), so fast weapons with light hits gain the most. Asad's rule
 * (2026-10-18; docs/bbcoop/papers_re.md has the reverse engineering):
 *   bonus = 80 x (base at +0 x multiplier) / 90  +  10% x (base at the current level x multiplier)
 * base = the weapon's EquipParamWeapon attackBasePhysics, multiplier = the attack's atkPhysCorrection / 100, or the
 * item's own element's correction for attacks with none (Holy Moonlight Sword beams); an attack with no multiplier at
 * all (a stamina-only guard break) gets 0, a weapon with base 0 at +0 keeps the vanilla +80.
 * A Saw Cleaver R1 (base 90 at +0, 100%) keeps +80 at +0. Stats and Blood gems do not change it.
 *
 * The game sums a character's active effects for one attack in 0x17e5a30 (rbx = the attack's struct:
 * +8 AtkParam id, +0xc its table: 1 = AtkParam_Pc, +0x84 the weapon id). For each effect that applies
 * (visceral attacks and other category 5 behaviors never get the papers), the loop has the effect's
 * +magic, +fire and +bolt in xmm4, xmm3 and xmm2 at 0x17e626e, already times the effect's four factors,
 * just before it adds them to the totals. The hook there scales the paper's value; the damage code
 * adds the totals after the multiplier, as for the vanilla +80.
 *   BB_PAPERS=vanilla (0, off, false)  the items' vanilla +80
 *   BB_PAPERS_TRACE=1                  logs the first bonus of each (paper, weapon, attack), up to 256 of them
 * Every site's bytes are checked before anything is written; any mismatch leaves the game vanilla. */
#define _GNU_SOURCE
#include "hooks.h"
#include "runtime_papers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EFFECT_SITE   0x17E626E /* vmovaps xmm5, [rbp-0x60]: one effect's adds are in xmm4/xmm3/xmm2 */
#define SPEFFECT_FIND 0x1F28AA0 /* (out {id, row* at +8}, SpEffectParam id) */
#define ATK_FIND      0x1F12FD0 /* (out {id, table, row* at +8}, table 0 Npc / 1 Pc, AtkParam id) */
#define WEAPON_FIND   0x1F29AA0 /* (out {id, EquipParamWeapon row* at +8, ..., reinforce}, weapon id) */

#define ATTACK_ATK_ID    0x08
#define ATTACK_ATK_TABLE 0x0C
#define ATTACK_WEAPON    0x84
#define ATK_TABLE_PC     1
#define ATK_PHYS_CORRECTION 0x3E /* u16, percent */
#define WEAPON_BASE_PHYSICS 0xC4 /* u16 */
#define SPEFFECT_STATE_INFO 0x156 /* u16 */

typedef struct { uint32_t id, table; const unsigned char *row; uint64_t extra[5]; } Found;
typedef void (*FindEffect)(Found *out, int32_t id);
typedef void (*FindAtk)(Found *out, int32_t table, int32_t id);
typedef void (*FindWeapon)(Found *out, int32_t id);

typedef struct {
    int32_t id;          /* SpEffectParam row */
    unsigned add;        /* its +magic, +fire or +bolt field */
    unsigned correction; /* the AtkParam correction of that element */
    unsigned lane;       /* the xmm register that value is in at the site */
    uint16_t state_info; /* its weapon effect (60 arcane, 61 bolt, 62 fire) */
    const char *name;
} Paper;

static const Paper papers[] = {
    {2200, 0x60, 0x42, 3, 62, "Fire Paper"},
    {2220, 0x64, 0x44, 2, 61, "Bolt Paper"},
    {2210, 0x5C, 0x40, 4, 60, "Empty Phantasm Shell"},
};
#define PAPER_COUNT (sizeof(papers) / sizeof(papers[0]))

typedef struct {
    const char *name;
    uint64_t offset;
    unsigned length;
    unsigned char original[24];
} Site;

static const Site sites[] = {
    {"effect adds site", EFFECT_SITE, 5, {0xC5,0xF8,0x28,0x6D,0xA0}},
    /* What the site relies on: vcvtsi2ss xmm4, [r14+0x5c]; xmm3, [r14+0x60]; xmm2, [r14+0x64] (each times
     * the effect's factors), and after the site: vaddss into the magic, fire and bolt totals. */
    {"magic add load", 0x17E61DE, 6, {0xC4,0xC1,0x7A,0x2A,0x66,0x5C}},
    {"fire add load", 0x17E6210, 6, {0xC4,0xC1,0x7A,0x2A,0x5E,0x60}},
    {"bolt add load", 0x17E6242, 6, {0xC4,0xC1,0x7A,0x2A,0x56,0x64}},
    {"magic total", 0x17E628D, 4, {0xC5,0xF2,0x58,0xCC}},
    {"fire total", 0x17E62A1, 4, {0xC5,0xF2,0x58,0xCB}},
    {"bolt total", 0x17E62B2, 4, {0xC5,0xF2,0x58,0xCA}},
    {"SpEffectParam lookup", SPEFFECT_FIND, 16,
     {0x55,0x48,0x89,0xE5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x41,0x89,0xF4,0x49,0x89}},
    {"AtkParam lookup", ATK_FIND, 16,
     {0x55,0x48,0x89,0xE5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x41,0x89,0xD7,0x89,0xF3}},
    {"EquipParamWeapon lookup", WEAPON_FIND, 17,
     {0x55,0x48,0x89,0xE5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x50,0x41,0x89,0xF4}},
};

static unsigned char *image_base;
static const unsigned char *paper_rows[PAPER_COUNT]; /* found once the params are loaded */
static int trace;
#define TRACED_MAX 256
static uint64_t traced[TRACED_MAX]; /* (paper, weapon, attack) keys logged, 0 = free */

float papers_bonus(float value, float base0, float base_now, float multiplier) {
    return value * base0 * multiplier / PAPERS_REFERENCE_BASE + PAPERS_SHARE * base_now * multiplier;
}

static uint16_t u16_at(const unsigned char *p, unsigned offset) { uint16_t v; memcpy(&v, p + offset, 2); return v; }
static int32_t i32_at(const unsigned char *p, unsigned offset) { int32_t v; memcpy(&v, p + offset, 4); return v; }

/* The paper this SpEffectParam row is, or -1. Rows are looked up until all three are found (the params
 * load after the hook goes in); a row whose weapon effect is not the paper's (another param mod) is left alone. */
static int paper_of(const unsigned char *row) {
    for (size_t i = 0; i < PAPER_COUNT; ++i) {
        const unsigned char *known = __atomic_load_n(&paper_rows[i], __ATOMIC_ACQUIRE);
        if (!known) {
            Found found = {0};
            ((FindEffect)(void *)(image_base + SPEFFECT_FIND))(&found, papers[i].id);
            if (!found.row || u16_at(found.row, SPEFFECT_STATE_INFO) != papers[i].state_info) continue;
            known = found.row;
            __atomic_store_n(&paper_rows[i], known, __ATOMIC_RELEASE);
        }
        if (row == known) return (int)i;
    }
    return -1;
}

/* Whether `key` is new to the trace (and now taken); keys past TRACED_MAX are not logged. */
static int first_time(uint64_t key) {
    for (size_t i = 0; i < TRACED_MAX; ++i) {
        uint64_t seen = __atomic_load_n(&traced[i], __ATOMIC_RELAXED);
        if (seen == key) return 0;
        if (!seen && __atomic_compare_exchange_n(&traced[i], &seen, key, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return 1;
        if (seen == key) return 0; /* another thread took this slot for the same key */
    }
    return 0;
}

static void effect_adds(HookRegs *regs) {
    const unsigned char *row = (const unsigned char *)(uintptr_t)regs->r14;
    const unsigned char *attack = (const unsigned char *)(uintptr_t)regs->rbx;
    if (!row || !attack) return;
    int which = paper_of(row);
    if (which < 0) return;
    const Paper *paper = &papers[which];
    int32_t value = i32_at(row, paper->add);
    int32_t atk_id = i32_at(attack, ATTACK_ATK_ID), weapon = i32_at(attack, ATTACK_WEAPON);
    if (value <= 0 || i32_at(attack, ATTACK_ATK_TABLE) != ATK_TABLE_PC || atk_id < 0 || weapon < 0) return;

    Found atk = {0}, now = {0}, plus0 = {0};
    ((FindAtk)(void *)(image_base + ATK_FIND))(&atk, ATK_TABLE_PC, atk_id);
    ((FindWeapon)(void *)(image_base + WEAPON_FIND))(&now, weapon);
    ((FindWeapon)(void *)(image_base + WEAPON_FIND))(&plus0, weapon - weapon % 10000);
    if (!atk.row || !now.row || !plus0.row) return; /* not a player weapon's attack: vanilla */
    uint16_t correction = u16_at(atk.row, ATK_PHYS_CORRECTION);
    if (!correction) correction = u16_at(atk.row, paper->correction);
    float multiplier = correction / 100.0f;
    float base0 = u16_at(plus0.row, WEAPON_BASE_PHYSICS), base_now = u16_at(now.row, WEAPON_BASE_PHYSICS);
    if (base0 <= 0.0f) return;

    float *lane = hook_saved_xmm(regs, paper->lane);
    float before = lane[0];
    lane[0] = before * papers_bonus((float)value, base0, base_now, multiplier) / (float)value;

    if (trace && first_time((uint64_t)(which + 1) << 60 | (uint64_t)(uint32_t)weapon << 28 | (uint32_t)atk_id))
        printf("Papers: %s, weapon %d attack %d: multiplier %.2f, base %g at +0 and %g now: +%.1f instead of +%.1f\n",
               paper->name, weapon, atk_id, multiplier, base0, base_now, lane[0], before);
}

const char *papers_install(unsigned char *image, uint64_t size) {
    const char *env = getenv("BB_PAPERS");
    if (env && (!strcmp(env, "vanilla") || !strcmp(env, "0") || !strcmp(env, "off") || !strcmp(env, "false"))) {
        printf("Papers: vanilla +80 (BB_PAPERS=%s)\n", env);
        return NULL;
    }
    for (size_t i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i) {
        const Site *s = &sites[i];
        if (s->offset > size || s->length > size - s->offset || memcmp(image + s->offset, s->original, s->length)) {
            printf("Papers: vanilla: site \"%s\" at %#llx does not match Bloodborne 1.09\n", s->name,
                   (unsigned long long)s->offset);
            return NULL;
        }
    }
    image_base = image;
    trace = (env = getenv("BB_PAPERS_TRACE")) && !strcmp(env, "1");
    /* Nothing is written when the stub area or the hook fails. Only a failed seal, after the jump is in
     * place, stops the game. */
    const char *failure = hooks_init(image, size);
    if (!failure) failure = hooks_install(EFFECT_SITE, sites[0].original, sites[0].length, effect_adds);
    if (failure) {
        printf("Papers: vanilla: %s\n", failure);
        return NULL;
    }
    if ((failure = hooks_seal())) return failure;
    printf("Papers: Fire/Bolt Paper and Empty Phantasm Shell scale with the weapon and the attack%s\n",
           trace ? " (BB_PAPERS_TRACE)" : "");
    return NULL;
}
