/* bbcoop: respec at the Doll (runtime_respec.c, runtime_respec_rules.c). docs/respec_re.md has
 * the reverse-engineering behind every offset and value. */
#ifndef BB_RESPEC_H
#define BB_RESPEC_H
#include <stdint.h>

#define RESPEC_STAT_COUNT 6 /* Vitality, Endurance, Strength, Skill, Bloodtinge, Arcane */
#define RESPEC_MAX_UPGRADE 10
#define RESPEC_MATERIAL_KINDS 4
#define RESPEC_GOODS_SHARD 3000
#define RESPEC_GOODS_TWIN_SHARD 3010
#define RESPEC_GOODS_CHUNK 3020
#define RESPEC_GOODS_ROCK 3030

typedef struct {
    int32_t level;
    int32_t stat[RESPEC_STAT_COUNT];
} RespecStats;

typedef struct {
    int32_t goods, count;
} RespecMaterial;

/* The game's echo cost of reaching `level` from level - 1. */
typedef int32_t (*RespecLevelCost)(int32_t level);

/* The starting level and stats of origin 0-8, or NULL. */
const RespecStats *respec_origin(unsigned origin);
/* Stats in 1-99 and level = their sum - 50, as the game keeps them. */
int respec_stats_valid(const RespecStats *stats);
/* Echoes spent on levels origin_level+1 .. level; -1 if a cost is negative. */
int64_t respec_echo_refund(int origin_level, int level, RespecLevelCost cost);
/* Upgrade level 0-10 of an EquipParamWeapon id (family*10000 + level*100), or -1. */
int respec_weapon_level(int32_t weapon_id);
/* The +0 id of a weapon, or -1. */
int32_t respec_weapon_base(int32_t weapon_id);
/* Materials spent from +0 to +level, one entry per goods kind used; returns the entry count, or -1. */
int respec_upgrade_materials(int level, RespecMaterial out[RESPEC_MATERIAL_KINDS]);

/* Installs the respec hooks (runtime_respec.c); NULL or an error. */
const char *respec_install(unsigned char *image, uint64_t size);
/* The menu text hook is in (runtime_saveslots.c's Duplicate needs it). */
int respec_text_hook_installed(void);
#endif
