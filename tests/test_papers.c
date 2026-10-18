/* bbcoop: runtime_papers.c on a synthetic image (x86-64 Linux): the maths, the all-or-nothing site check,
 * BB_PAPERS=vanilla, and the hook called for real. The three param lookups are stand-ins that start with
 * the game's checked bytes and then call the tables below, so the handler's lookups and register changes
 * run as in the game. */
#define _GNU_SOURCE
#include "runtime_papers.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define SIZE 0x02000000
#define SITE 0x17E626E

typedef struct { uint32_t id, table; const unsigned char *row; uint64_t extra[5]; } Found;

static unsigned char fire_row[0x210], bolt_row[0x210], shell_row[0x210], other_row[0x210];
static unsigned char atk_r1[0x80], atk_sword_r1[0x80], atk_beam[0x80], atk_guard_break[0x80];
static unsigned char cleaver0[0x100], cleaver10[0x100], kirk0[0x100], kirk10[0x100], baseless0[0x100];
static int params_loaded; /* the SpEffectParam lookup finds nothing until the params are loaded */

static void put16(unsigned char *row, unsigned offset, uint16_t v) { memcpy(row + offset, &v, 2); }
static void put32(unsigned char *row, unsigned offset, int32_t v) { memcpy(row + offset, &v, 4); }

static void find_effect(Found *out, int32_t id) {
    out->id = (uint32_t)id;
    out->row = !params_loaded ? NULL : id == 2200 ? fire_row : id == 2220 ? bolt_row : id == 2210 ? shell_row : NULL;
}
static void find_atk(Found *out, int32_t table, int32_t id) {
    out->id = (uint32_t)id;
    out->table = (uint32_t)table;
    out->row = table != 1 ? NULL : id == 700000 ? atk_r1 : id == 800000 ? atk_sword_r1 : id == 2600590 ? atk_beam
             : id == 700300 ? atk_guard_break : NULL;
}
static void find_weapon(Found *out, int32_t id) {
    out->id = (uint32_t)id;
    id = id / 100 * 100;
    out->row = id == 7000000 ? cleaver0 : id == 7001000 ? cleaver10 : id == 8000000 ? kirk0 : id == 8001000 ? kirk10
             : id == 38000000 ? baseless0 : NULL;
}

static unsigned char *image;
static void put(uint64_t offset, const void *bytes, size_t length) { memcpy(image + offset, bytes, length); }
static unsigned char *call_c(unsigned char *p, void *fn) {
    memcpy(p, "\x48\xB8", 2); memcpy(p + 2, &fn, 8); memcpy(p + 10, "\xFF\xD0", 2); /* mov rax, fn; call rax */
    return p + 12;
}
static void original(void) {
    put(SITE, "\xC5\xF8\x28\x6D\xA0", 5);
    put(SITE + 5, "\xC3", 1); /* the stub jumps back here: return to run_site */
    put(0x17E61DE, "\xC4\xC1\x7A\x2A\x66\x5C", 6);
    put(0x17E6210, "\xC4\xC1\x7A\x2A\x5E\x60", 6);
    put(0x17E6242, "\xC4\xC1\x7A\x2A\x56\x64", 6);
    put(0x17E628D, "\xC5\xF2\x58\xCC", 4);
    put(0x17E62A1, "\xC5\xF2\x58\xCB", 4);
    put(0x17E62B2, "\xC5\xF2\x58\xCA", 4);
    /* SpEffectParam: push rbp .. push rbx; mov r12d, esi; mov r14, rdi -> find_effect(r14, r12d) */
    unsigned char *p = image + 0x1F28AA0;
    memcpy(p, "\x55\x48\x89\xE5\x41\x57\x41\x56\x41\x54\x53\x41\x89\xF4\x49\x89\xFE", 17);
    memcpy(p + 17, "\x4C\x89\xF7\x44\x89\xE6", 6); /* mov rdi, r14; mov esi, r12d */
    p = call_c(p + 23, (void *)find_effect);
    memcpy(p, "\x5B\x41\x5C\x41\x5E\x41\x5F\x5D\xC3", 9);
    /* AtkParam: push rbp .. push rbx; mov r15d, edx; mov ebx, esi -> find_atk(rdi, esi, edx) */
    p = image + 0x1F12FD0;
    memcpy(p, "\x55\x48\x89\xE5\x41\x57\x41\x56\x41\x54\x53\x41\x89\xD7\x89\xF3", 16);
    p = call_c(p + 16, (void *)find_atk);
    memcpy(p, "\x5B\x41\x5C\x41\x5E\x41\x5F\x5D\xC3", 9);
    /* EquipParamWeapon: push rbp .. push rax; mov r12d, esi -> find_weapon(rdi, esi) */
    p = image + 0x1F29AA0;
    memcpy(p, "\x55\x48\x89\xE5\x41\x57\x41\x56\x41\x55\x41\x54\x53\x50\x41\x89\xF4", 17);
    p = call_c(p + 17, (void *)find_weapon);
    memcpy(p, "\x48\x83\xC4\x08\x5B\x41\x5C\x41\x5D\x41\x5E\x41\x5F\x5D\xC3", 15);
}

static unsigned char *fresh(void) {
    image = mmap(NULL, SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(image != MAP_FAILED);
    original();
    return image;
}

/* Runs the site as the effect loop reaches it: rbx = the attack, r14 = the effect's row, xmm4/xmm3/xmm2 =
 * its +magic/+fire/+bolt (lanes[0..2], written back), rbp = frame (rbp-0x60 is read, 16-byte aligned). */
void run_site(void *site, const void *attack, const void *row, float *lanes, void *frame);
__asm__(
    ".text\n.globl run_site\n.type run_site, @function\nrun_site:\n"
    "    push %rbp\n push %rbx\n push %r14\n push %rcx\n"
    "    mov %rsi, %rbx\n mov %rdx, %r14\n"
    "    vmovss 0(%rcx), %xmm4\n vmovss 4(%rcx), %xmm3\n vmovss 8(%rcx), %xmm2\n"
    "    mov %r8, %rbp\n sub $8, %rsp\n call *%rdi\n add $8, %rsp\n"
    "    pop %rcx\n vmovss %xmm4, 0(%rcx)\n vmovss %xmm3, 4(%rcx)\n vmovss %xmm2, 8(%rcx)\n"
    "    pop %r14\n pop %rbx\n pop %rbp\n ret\n"
    ".size run_site, .-run_site\n");

static _Alignas(16) unsigned char frame[256];
static int near(float a, float b) { return fabsf(a - b) < 1e-3f; }

static void attack(unsigned char *a, int32_t table, int32_t atk, int32_t weapon) {
    memset(a, 0, 0xC0);
    put32(a, 0x08, atk);
    put32(a, 0x0C, table);
    put32(a, 0x84, weapon);
}
static void hit(const unsigned char *a, const unsigned char *row, float magic, float fire, float bolt, float out[3]) {
    out[0] = magic; out[1] = fire; out[2] = bolt;
    run_site(image + SITE, a, row, out, frame + 0x80);
}

int main(void) {
    unsetenv("BB_PAPERS");
    unsetenv("BB_PAPERS_TRACE");

    /* The maths: Saw Cleaver R1 keeps +80 at +0 (+9 for the 10%); Kirkhammer sword R1 at +10. */
    assert(near(papers_bonus(80.0f, 90.0f, 90.0f, 1.0f), 89.0f));
    assert(near(papers_bonus(80.0f, 90.0f, 180.0f, 1.0f), 98.0f));
    assert(near(papers_bonus(80.0f, 105.0f, 210.0f, 0.7f), 80.0f * 105.0f * 0.7f / 90.0f + 14.7f));
    assert(papers_bonus(80.0f, 90.0f, 180.0f, 0.0f) == 0.0f);

    /* Rows: the papers (weapon effects 62/61/60, +80 each), another effect, attacks, weapons. */
    put16(fire_row, 0x156, 62); put32(fire_row, 0x60, 80);
    put16(bolt_row, 0x156, 61); put32(bolt_row, 0x64, 80);
    put16(shell_row, 0x156, 60); put32(shell_row, 0x5C, 80);
    put16(other_row, 0x156, 62); put32(other_row, 0x60, 80);
    put16(atk_r1, 0x3E, 100);
    put16(atk_sword_r1, 0x3E, 70);
    put16(atk_beam, 0x40, 280); put16(atk_beam, 0x42, 200); put16(atk_beam, 0x44, 180); /* no physical */
    put16(cleaver0, 0xC4, 90); put16(cleaver10, 0xC4, 180);
    put16(kirk0, 0xC4, 105); put16(kirk10, 0xC4, 210);

    /* Any site that differs: vanilla, nothing written. */
    fresh();
    image[0x17E62A1] ^= 1;
    assert(!papers_install(image, SIZE));
    assert(image[SITE] == 0xC5);
    munmap(image, SIZE);

    /* BB_PAPERS=vanilla (or 0, off, false): nothing written. */
    static const char *const offs[] = {"vanilla", "0", "off", "false"};
    for (unsigned i = 0; i < 4; ++i) {
        fresh();
        setenv("BB_PAPERS", offs[i], 1);
        assert(!papers_install(image, SIZE));
        assert(image[SITE] == 0xC5);
        munmap(image, SIZE);
    }
    unsetenv("BB_PAPERS");

    /* Default (with the trace, which logs each paper/weapon/attack once): the site jumps to the hook. */
    fresh();
    setenv("BB_PAPERS_TRACE", "1", 1);
    assert(!papers_install(image, SIZE));
    assert(image[SITE] == 0xE9);

    unsigned char a[0xC0];
    float out[3];
    /* Before the params are loaded no row is a paper; the lookup is tried again on the next effect. */
    attack(a, 1, 700000, 7001000);
    hit(a, fire_row, 0.0f, 80.0f, 0.0f, out);
    assert(out[1] == 80.0f);
    params_loaded = 1;
    /* Fire Paper on a Saw Cleaver +10 R1, effect factors 1.5: 1.5 x (80 + 18); the other adds untouched. */
    attack(a, 1, 700000, 7001000);
    hit(a, fire_row, 3.0f, 120.0f, 5.0f, out);
    assert(near(out[1], 1.5f * 98.0f) && out[0] == 3.0f && out[2] == 5.0f);
    hit(a, fire_row, 0.0f, 80.0f, 0.0f, out); /* same pair: no second trace line */
    assert(near(out[1], 98.0f));
    /* Bolt Paper and the Shell use their own registers. */
    hit(a, bolt_row, 0.0f, 0.0f, 80.0f, out);
    assert(near(out[2], 98.0f) && out[1] == 0.0f);
    hit(a, shell_row, 80.0f, 0.0f, 0.0f, out);
    assert(near(out[0], 98.0f));
    /* Kirkhammer sword R1 (70%) at +0 and at +10. */
    attack(a, 1, 800000, 8000000);
    hit(a, fire_row, 0.0f, 80.0f, 0.0f, out);
    assert(near(out[1], papers_bonus(80.0f, 105.0f, 105.0f, 0.7f)));
    attack(a, 1, 800000, 8001000);
    hit(a, fire_row, 0.0f, 80.0f, 0.0f, out);
    assert(near(out[1], papers_bonus(80.0f, 105.0f, 210.0f, 0.7f)));
    /* No physical multiplier: the item's own element's (Fire Paper: 200%, Bolt Paper: 180%). */
    attack(a, 1, 2600590, 7001000);
    hit(a, fire_row, 0.0f, 80.0f, 0.0f, out);
    assert(near(out[1], papers_bonus(80.0f, 90.0f, 180.0f, 2.0f)));
    hit(a, bolt_row, 0.0f, 0.0f, 80.0f, out);
    assert(near(out[2], papers_bonus(80.0f, 90.0f, 180.0f, 1.8f)));
    /* No multiplier at all (a stamina-only guard break): no bonus. */
    attack(a, 1, 700300, 7001000);
    hit(a, fire_row, 0.0f, 80.0f, 0.0f, out);
    assert(out[1] == 0.0f);
    /* A weapon with base 0 at +0 keeps the vanilla +80. */
    attack(a, 1, 700000, 38000000);
    hit(a, fire_row, 0.0f, 80.0f, 0.0f, out);
    assert(out[1] == 80.0f);
    /* Left vanilla: another effect, an NPC attack table, an unknown attack or weapon, no weapon. */
    hit(a, other_row, 0.0f, 80.0f, 0.0f, out);
    assert(out[1] == 80.0f);
    attack(a, 0, 800000, 8001000);
    hit(a, fire_row, 0.0f, 80.0f, 0.0f, out);
    assert(out[1] == 80.0f);
    attack(a, 1, 123, 8001000);
    hit(a, fire_row, 0.0f, 80.0f, 0.0f, out);
    assert(out[1] == 80.0f);
    attack(a, 1, 800000, 9000000);
    hit(a, fire_row, 0.0f, 80.0f, 0.0f, out);
    assert(out[1] == 80.0f);
    attack(a, 1, 800000, -1);
    hit(a, fire_row, 0.0f, 80.0f, 0.0f, out);
    assert(out[1] == 80.0f);
    return 0;
}
