/* bbcoop: respec rules (runtime_respec_rules.c) and the install of runtime_respec.c on a synthetic
 * image (x86-64 Linux): the all-or-nothing site check, the hook, and the frame hook doing nothing
 * before the game is running. The handlers' game calls need the real game (tools/respec_test.sh). */
#define _GNU_SOURCE
#include "runtime_respec.h"
#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define SIZE 0x05600000
__thread sigjmp_buf *runtime_fault_recover;
const char *runtime_file_user_dir(void) { return "/nonexistent"; }
static unsigned char *image;

/* v1.09's GetLevelUpCost (0x157f050) in float32, as the game computes it. */
static int32_t game_cost(int32_t level) {
    float x = (float)(level + 80), extra = x - 92.0f;
    if (extra < 0.0f) extra = 0.0f;
    return (int32_t)(x * x * (0.1f + extra * 0.02f) + 1.0f + 0.5f);
}
static int32_t broken_cost(int32_t level) { return level == 12 ? -1 : 1; }

static void put(uint64_t offset, const void *bytes, size_t length) { memcpy(image + offset, bytes, length); }
static void original(void) {
    put(0x0187236D, "\x48\x81\xEC\xE8\x06\x00\x00", 7);
    put(0x013BCCF0, "\x85\xF6\x78\x67\x8B\x4F\x1C\x31\xD2\x89\xF0\xF7\xF1\x41\x89\xC0", 16);
    put(0x013CFCC0, "\x41\x89\xD0\x8B\x4F\x1C\x31\xD2\x89\xF0\xF7\xF1\x41\x89\xC1\x44", 16);
    put(0x0157F050, "\x55\x48\x89\xE5\x41\x56\x53\x48\x83\xEC\x20\x89\xFB\x4C\x8B\x35", 16);
    put(0x01C13D00, "\x55\x48\x89\xE5\x41\x57\x41\x56\x41\x55\x41\x54\x53\x48\x83\xEC", 16);
    put(0x01336DA0, "\x55\x48\x89\xE5\x48\x8D\x05\x1D\x43\x20\x04\x48\x83\x38\x00\x75", 16);
    put(0x013CDF30, "\x55\x48\x89\xE5\x41\x57\x41\x56\x53\x48\x83\xEC\x38\x89\xFB\x4C", 16);
    put(0x01BD2D73, "\x48\x8D\x7B\x20\x48\x8D\x75\xB0", 8);
    put(0x01C5E54D, "\xC7\x85\xF8\xFE\xFF\xFF\xFF\xFF\xFF\xFF", 10);
    put(0x01B5DD88, "\x41\x8B\x76\x48\x48\x89\xDF", 7);
    put(0x014DA3F0, "\x55\x48\x89\xE5\x41\x57\x41\x56\x41\x55\x41\x54\x53\x48\x81\xEC", 16);
    put(0x01A8BED0, "\x55\x48\x89\xE5\x41\x57\x41\x56\x53\x50\x49\x89\xF6\x48\x89\xFB", 16);
    put(0x014D1B30, "\x55\x48\x89\xE5\x41\x57\x41\x56\x41\x55\x41\x54\x53\x48\x81\xEC", 16);
    put(0x01B5C8C0, "\x55\x48\x89\xE5\x41\x57\x41\x56\x41\x54\x53\x48\x83\xEC\x30\x89", 16);
    put(0x0131CB70, "\x55\x48\x89\xE5\x41\x57\x41\x56\x41\x55\x41\x54\x53\x48\x83\xEC", 16);
}

static void rules(void) {
    /* Origins: level = stats - 50 for all nine; none past Waste of Skin. */
    for (unsigned o = 0; o < 9; ++o) assert(respec_origin(o) && respec_stats_valid(respec_origin(o)));
    assert(!respec_origin(9));
    assert(respec_origin(8)->level == 4 && respec_origin(6)->stat[4] == 14);
    RespecStats bad = *respec_origin(0);
    bad.level++;
    assert(!respec_stats_valid(&bad));

    /* Level costs: the game's values (docs/respec_re.md), and the refund sums. */
    assert(game_cost(11) == 829 && game_cost(12) == 847 && game_cost(13) == 1039);
    assert(game_cost(20) == 2601 && game_cost(100) == 60265);
    assert(respec_echo_refund(10, 10, game_cost) == 0);
    assert(respec_echo_refund(10, 12, game_cost) == 829 + 847);
    assert(respec_echo_refund(4, 5, game_cost) == 724);
    assert(respec_echo_refund(10, 20, broken_cost) == -1);

    /* Weapon ids: family*10000 + level*100; Uncanny +10000, Lost +20000. */
    assert(respec_weapon_level(7001000) == 10 && respec_weapon_base(7001000) == 7000000);
    assert(respec_weapon_level(7020300) == 3 && respec_weapon_base(7020300) == 7020000);
    assert(respec_weapon_level(14000000) == 0);
    assert(respec_weapon_level(7001100) == -1 && respec_weapon_level(7000050) == -1 && respec_weapon_level(-5) == -1);

    /* Materials: +0 to +10 is 16 Shards, 16 Twin Shards, 16 Chunks and 1 Rock. */
    RespecMaterial m[RESPEC_MATERIAL_KINDS];
    assert(respec_upgrade_materials(0, m) == 0);
    assert(respec_upgrade_materials(2, m) == 1 && m[0].goods == RESPEC_GOODS_SHARD && m[0].count == 8);
    assert(respec_upgrade_materials(5, m) == 2 && m[0].count == 16 && m[1].goods == RESPEC_GOODS_TWIN_SHARD &&
           m[1].count == 8);
    assert(respec_upgrade_materials(10, m) == 4);
    assert(m[0].count == 16 && m[1].count == 16 && m[2].goods == RESPEC_GOODS_CHUNK && m[2].count == 16);
    assert(m[3].goods == RESPEC_GOODS_ROCK && m[3].count == 1);
    assert(respec_upgrade_materials(11, m) == -1);
}

int main(void) {
    rules();
    image = mmap(NULL, SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    unsigned char *copy = malloc(SIZE);
    assert(image != MAP_FAILED && copy);

    /* A blank (wrong) build: refused, nothing written. */
    assert(respec_install(image, SIZE) != NULL);
    for (size_t i = 0; i < SIZE; ++i) assert(image[i] == 0);

    /* One changed game function: refused, naming it, still nothing written. */
    original();
    image[0x01C13D00 + 4] = 0x90;
    memcpy(copy, image, SIZE);
    const char *error = respec_install(image, SIZE);
    assert(error && strstr(error, "LevelUpCommit"));
    assert(!memcmp(copy, image, SIZE));
    image[0x01C13D00 + 4] = 0x41;

    /* BB_RESPEC=0: nothing installed. */
    setenv("BB_RESPEC", "0", 1);
    assert(respec_install(image, SIZE) == NULL && image[0x0187236D] == 0x48);
    unsetenv("BB_RESPEC");

    /* The hooks. After the frame site (sub rsp,0x6e8) the test code undoes it and returns. With
     * no game running (null globals) the handler does nothing. (SetEventFlag's body needs the
     * game's flag manager, so it is not called here.) */
    put(0x01872374, "\x48\x81\xC4\xE8\x06\x00\x00\xC3", 8);
    assert(respec_install(image, SIZE) == NULL);
    assert(image[0x0187236D] == 0xE9 && image[0x013CFCC0] == 0xE9);
    assert(image[0x01BD2D73] == 0xE9 && image[0x01C5E54D] == 0xE9 && image[0x01B5DD88] == 0xE9);
    assert(respec_install(image, SIZE) != NULL); /* already hooked */
    ((void (*)(void))(image + 0x0187236D))();
    return 0;
}
