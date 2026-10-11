/* bbcoop: runtime_camera.c on a synthetic image (x86-64 Linux): the all-or-nothing site check, the
 * turn-ramp patch, the update hook (called for real: rdi and xmm0 reach the handler), the env
 * switches, and the chase-rate maths. */
#define _GNU_SOURCE
#include "runtime_camera.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define SIZE 0x01500000
static const unsigned char ramp[10] = {0xC7,0x83,0xE8,0x01,0x00,0x00,0x9A,0x99,0x19,0x3F};
static const unsigned char ramp_off[10] = {0xC7,0x83,0xE8,0x01,0x00,0x00,0x28,0x6B,0x6E,0x4E};
static const unsigned char entry[10] = {0x55,0x48,0x89,0xE5,0x41,0x57,0x41,0x56,0x41,0x55};

/* The update's prologue is followed by its mirror: pop r13; pop r14; pop r15; pop rbp; ret. */
static const unsigned char epilogue[7] = {0x41,0x5D,0x41,0x5E,0x41,0x5F,0x5D};
/* The update's reads of the five fractions (vmovss/vmulss [r13 + field]). */
static const struct { unsigned field; unsigned offset; unsigned char bytes[9]; } reads[] = {
    {0, 0x143c30f, {0xC4,0x41,0x7A,0x10,0x95,0x40,0x02,0x00,0x00}},
    {0, 0x143c784, {0xC4,0xC1,0x7A,0x10,0xAD,0x40,0x02,0x00,0x00}},
    {1, 0x143cef0, {0xC4,0xC1,0x7A,0x59,0x85,0xA8,0x01,0x00,0x00}},
    {2, 0x143f97e, {0xC4,0xC1,0x7A,0x59,0x85,0x3C,0x02,0x00,0x00}},
    {2, 0x143fa9f, {0xC4,0xC1,0x72,0x59,0x8D,0x3C,0x02,0x00,0x00}},
    {3, 0x143c9e6, {0xC4,0xC1,0x7A,0x10,0x95,0x88,0x02,0x00,0x00}},
    {4, 0x143ae75, {0xC4,0xC1,0x7A,0x10,0xA5,0x30,0x03,0x00,0x00}},
};

static unsigned char *fresh(void) {
    unsigned char *image = mmap(NULL, SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(image != MAP_FAILED);
    memcpy(image + 0x143A57B, ramp, 10);
    memcpy(image + 0x143A979, ramp, 10);
    memcpy(image + 0x143AC60, entry, 10);
    memcpy(image + 0x143AC6A, epilogue, 7);
    image[0x143AC71] = 0xC3;
    for (unsigned i = 0; i < sizeof(reads) / sizeof(reads[0]); ++i) memcpy(image + reads[i].offset, reads[i].bytes, 9);
    return image;
}

static float field(const unsigned char *camera, unsigned i) {
    float value;
    memcpy(&value, camera + camera_rate_fields[i], 4);
    return value;
}
static void set_field(unsigned char *camera, unsigned i, float value) { memcpy(camera + camera_rate_fields[i], &value, 4); }
static int near(float a, float b) { return fabsf(a - b) < 1e-5f; }

static void install(void) {
    /* Default: both fixes. */
    unsigned char *image = fresh();
    assert(!camera_install(image, SIZE));
    assert(!memcmp(image + 0x143A57B, ramp_off, 10) && !memcmp(image + 0x143A979, ramp_off, 10));
    assert(image[0x143AC60] == 0xE9 && image[0x143AC65] == 0x90 && image[0x143AC69] == 0x90);

    /* The hooked update, called as the game does: rdi = camera, xmm0 = frame time. */
    unsigned char camera[0x400] = {0};
    set_field(camera, 0, 0.3f);
    void (*update)(unsigned char *, float) = (void (*)(unsigned char *, float))(void *)(image + 0x143AC60);
    update(camera, 1.0f / 120.0f);
    assert(near(field(camera, 0), 1.0f - powf(0.7f, 0.25f)));

    /* One site differs: nothing is written. */
    image = fresh();
    image[0x143A97F] ^= 1;
    assert(!camera_install(image, SIZE));
    assert(!memcmp(image + 0x143A57B, ramp, 10) && !memcmp(image + 0x143AC60, entry, 10));
    image = fresh();
    image[0x143AC61] ^= 1;
    assert(!camera_install(image, SIZE));
    assert(!memcmp(image + 0x143A57B, ramp, 10));

    /* BB_CAMERA_ACCEL=1 keeps the ramp; BB_CAMERA_RATES=0 skips the hook; both: untouched. */
    setenv("BB_CAMERA_ACCEL", "1", 1);
    image = fresh();
    assert(!camera_install(image, SIZE));
    assert(!memcmp(image + 0x143A57B, ramp, 10) && image[0x143AC60] == 0xE9);
    setenv("BB_CAMERA_RATES", "0", 1);
    image = fresh();
    assert(!camera_install(image, SIZE));
    assert(!memcmp(image + 0x143A57B, ramp, 10) && !memcmp(image + 0x143AC60, entry, 10));
    unsetenv("BB_CAMERA_ACCEL");
    image = fresh();
    assert(!camera_install(image, SIZE));
    assert(!memcmp(image + 0x143A57B, ramp_off, 10) && !memcmp(image + 0x143AC60, entry, 10));
    unsetenv("BB_CAMERA_RATES");

    /* The current Uncap FPS++ replaces the reads of fields 0, 1, 2 and 4 with jumps: only field 3 is
     * rescaled. */
    image = fresh();
    for (unsigned i = 0; i < sizeof(reads) / sizeof(reads[0]); ++i)
        if (reads[i].field != 3) memcpy(image + reads[i].offset, "\xE9\x00\x00\x00\x00\x90\x90\x90\x90", 9);
    assert(!camera_install(image, SIZE));
    assert(image[0x143AC60] == 0xE9 && !memcmp(image + 0x143A57B, ramp_off, 10));
    unsigned char patched[0x400] = {0};
    for (unsigned i = 0; i < CAMERA_RATE_FIELDS; ++i) set_field(patched, i, 0.3f);
    update(patched, 1.0f / 120.0f);
    for (unsigned i = 0; i < CAMERA_RATE_FIELDS; ++i)
        assert(near(field(patched, i), i == 3 ? 1.0f - powf(0.7f, 0.25f) : 0.3f));

    /* Every read taken: no hook, the ramp patch only. */
    image = fresh();
    for (unsigned i = 0; i < sizeof(reads) / sizeof(reads[0]); ++i) image[reads[i].offset] = 0xE9;
    assert(!camera_install(image, SIZE));
    assert(!memcmp(image + 0x143AC60, entry, 10) && !memcmp(image + 0x143A57B, ramp_off, 10));

    /* Back to every field for rates(). */
    assert(!camera_install(fresh(), SIZE));
}

static void rates(void) {
    static const float vanilla[CAMERA_RATE_FIELDS] = {0.3f, 0.3f, 0.06f, 0.3f, 0.05f};
    unsigned char camera[0x400] = {0}, other[0x400] = {0};
    for (unsigned i = 0; i < CAMERA_RATE_FIELDS; ++i) set_field(camera, i, vanilla[i]), set_field(other, i, vanilla[i]);

    /* 30 FPS: the game's own fractions. */
    camera_rates_apply(camera, 1.0f / 30.0f);
    for (unsigned i = 0; i < CAMERA_RATE_FIELDS; ++i) assert(near(field(camera, i), vanilla[i]));

    /* 120 FPS: four frames cover what one 30 FPS frame did, from the base, not compounded. */
    for (int frame = 0; frame < 3; ++frame) camera_rates_apply(camera, 1.0f / 120.0f);
    float r = field(camera, 0);
    assert(near(r, 1.0f - powf(0.7f, 0.25f)));
    assert(near(1.0f - powf(1.0f - r, 4.0f), 0.3f));
    assert(near(field(camera, 4), 1.0f - powf(0.95f, 0.25f)));

    /* The game sets a new value: it becomes the base. */
    set_field(camera, 4, 0.2f);
    camera_rates_apply(camera, 1.0f / 30.0f);
    assert(near(field(camera, 4), 0.2f) && near(field(camera, 0), 0.3f));

    /* A second camera keeps its own bases. */
    camera_rates_apply(other, 1.0f / 60.0f);
    assert(near(field(other, 1), 1.0f - powf(0.7f, 0.5f)) && near(field(camera, 1), 0.3f));

    /* Out-of-range frame times and fractions are left alone. */
    camera_rates_apply(camera, 0.0f);
    camera_rates_apply(camera, 1.0f);
    camera_rates_apply(camera, NAN);
    assert(near(field(camera, 0), 0.3f));
    set_field(camera, 2, 1.5f);
    camera_rates_apply(camera, 1.0f / 120.0f);
    assert(field(camera, 2) == 1.5f);

    /* Uncap FPS++'s DirectPitchRate (0.03) stands for the vanilla 0.06. */
    set_field(camera, 2, 0.03f);
    camera_rates_apply(camera, 1.0f / 30.0f);
    assert(near(field(camera, 2), 0.06f));
    camera_rates_apply(camera, 1.0f / 120.0f);
    assert(near(field(camera, 2), 1.0f - powf(0.94f, 0.25f)));

    /* Eight more cameras seen once each do not evict the one updated every frame. */
    static unsigned char others[8][0x400];
    for (unsigned c = 0; c < 8; ++c) {
        for (unsigned i = 0; i < CAMERA_RATE_FIELDS; ++i) set_field(others[c], i, vanilla[i]);
        camera_rates_apply(others[c], 1.0f / 120.0f);
        camera_rates_apply(camera, 1.0f / 120.0f);
    }
    assert(near(field(camera, 0), 1.0f - powf(0.7f, 0.25f)));
}

int main(void) {
    unsetenv("BB_CAMERA_ACCEL");
    unsetenv("BB_CAMERA_RATES");
    install();
    rates();
    return 0;
}
