/* bbcoop: respec rules (no game access, tested by tests/test_respec.c). Values are v1.09's own:
 * CharaInitParam rows 3000-3008 and EquipMtrlSetParam rows 100-1000 (docs/bbcoop/respec_re.md). */
#include "runtime_respec.h"
#include <stddef.h>

static const RespecStats origins[] = {
    {10, {11, 10, 12, 10,  9,  8}}, /* Milquetoast */
    {10, {14, 11, 11, 10,  7,  7}}, /* Lone Survivor */
    {10, { 9, 14,  9, 13,  6,  9}}, /* Troubled Childhood */
    {10, {12, 11, 15,  9,  6,  7}}, /* Violent Past */
    {10, { 9, 12,  9, 15,  7,  8}}, /* Professional */
    {10, {10, 10, 14, 13,  7,  6}}, /* Military Veteran */
    {10, { 7,  8,  9, 13, 14,  9}}, /* Noble Scion */
    {10, {10, 12, 10,  9,  5, 14}}, /* Cruel Fate */
    { 4, {10,  9, 10,  9,  7,  9}}, /* Waste of Skin */
};

const RespecStats *respec_origin(unsigned origin) {
    return origin < sizeof(origins) / sizeof(origins[0]) ? &origins[origin] : NULL;
}

int respec_stats_valid(const RespecStats *s) {
    int sum = 0;
    for (int i = 0; i < RESPEC_STAT_COUNT; ++i) {
        if (s->stat[i] < 1 || s->stat[i] > 99) return 0;
        sum += s->stat[i];
    }
    return s->level == sum - 50; /* every level is one stat point */
}

int64_t respec_echo_refund(int origin_level, int level, RespecLevelCost cost) {
    int64_t total = 0;
    for (int n = origin_level + 1; n <= level; ++n) {
        int32_t c = cost(n);
        if (c < 0) return -1;
        total += c;
    }
    return total;
}

/* Step to +n costs row n*100: count of goods. */
static const RespecMaterial steps[RESPEC_MAX_UPGRADE + 1] = {
    {0, 0},
    {RESPEC_GOODS_SHARD, 3}, {RESPEC_GOODS_SHARD, 5}, {RESPEC_GOODS_SHARD, 8},
    {RESPEC_GOODS_TWIN_SHARD, 3}, {RESPEC_GOODS_TWIN_SHARD, 5}, {RESPEC_GOODS_TWIN_SHARD, 8},
    {RESPEC_GOODS_CHUNK, 3}, {RESPEC_GOODS_CHUNK, 5}, {RESPEC_GOODS_CHUNK, 8},
    {RESPEC_GOODS_ROCK, 1},
};

int respec_weapon_level(int32_t weapon_id) {
    if (weapon_id < 0) return -1;
    int level = (weapon_id % 10000) / 100;
    return weapon_id % 100 == 0 && level <= RESPEC_MAX_UPGRADE ? level : -1;
}

int32_t respec_weapon_base(int32_t weapon_id) {
    int level = respec_weapon_level(weapon_id);
    return level < 0 ? -1 : weapon_id - level * 100;
}

int respec_upgrade_materials(int level, RespecMaterial out[RESPEC_MATERIAL_KINDS]) {
    static const int32_t kinds[RESPEC_MATERIAL_KINDS] = {RESPEC_GOODS_SHARD, RESPEC_GOODS_TWIN_SHARD,
                                                         RESPEC_GOODS_CHUNK, RESPEC_GOODS_ROCK};
    if (level < 0 || level > RESPEC_MAX_UPGRADE) return -1;
    int count = 0;
    for (int k = 0; k < RESPEC_MATERIAL_KINDS; ++k) {
        int32_t total = 0;
        for (int n = 1; n <= level; ++n)
            if (steps[n].goods == kinds[k]) total += steps[n].count;
        if (total) out[count++] = (RespecMaterial){kinds[k], total};
    }
    return count;
}
