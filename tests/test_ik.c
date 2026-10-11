/* bbcoop: runtime_ik.c on a synthetic image (x86-64 Linux): the all-or-nothing site check, the update
 * hook (called for real: xmm0 reaches the handler and the gains follow it), BB_IK_HEIGHT and the maths. */
#define _GNU_SOURCE
#include "runtime_ik.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define SIZE 0x05130000
static const unsigned char entry[10] = {0x55,0x48,0x89,0xE5,0x41,0x57,0x41,0x56,0x41,0x55};
/* The update's prologue is followed by its mirror: pop r13; pop r14; pop r15; pop rbp; ret. */
static const unsigned char epilogue[8] = {0x41,0x5D,0x41,0x5E,0x41,0x5F,0x5D,0xC3};
static const unsigned char on_step[8] = {0xC5,0xEA,0x59,0x05,0x9E,0x29,0xC1,0x03};
static const unsigned char off_step[8] = {0xC5,0xFA,0x10,0x05,0x8F,0x29,0xC1,0x03};

static unsigned char *fresh(void) {
    unsigned char *image = mmap(NULL, SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(image != MAP_FAILED);
    memcpy(image + 0x1514DA0, entry, 10);
    memcpy(image + 0x1514DAA, epilogue, 8);
    memcpy(image + 0x1515182, on_step, 8);
    memcpy(image + 0x1515195, off_step, 8);
    float on = 0.1f, off = 0.6f;
    memcpy(image + 0x5127B28, &on, 4);
    memcpy(image + 0x5127B2C, &off, 4);
    return image;
}

static float gain(const unsigned char *image, unsigned i) {
    float value;
    memcpy(&value, image + 0x5127B28 + 4 * i, 4);
    return value;
}
static int near(float a, float b) { return fabsf(a - b) < 1e-5f; }

int main(void) {
    unsetenv("BB_IK_HEIGHT");

    /* The maths: the game's gains at 30 FPS; at 120 FPS four frames do what one did. */
    float on, off;
    ik_gains_for(1.0f / 30.0f, &on, &off);
    assert(near(on, 0.1f) && near(off, 0.6f));
    ik_gains_for(1.0f / 120.0f, &on, &off);
    assert(near(1.0f - powf(1.0f - on, 4.0f), 0.1f) && near(powf(off, 4.0f), 0.6f));

    /* Default: the hook, called as the game does (xmm0 = frame time), sets the gains. */
    unsigned char *image = fresh();
    assert(!ik_install(image, SIZE));
    assert(image[0x1514DA0] == 0xE9 && image[0x1514DA5] == 0x90 && image[0x1514DA9] == 0x90);
    void (*update)(void *, float) = (void (*)(void *, float))(void *)(image + 0x1514DA0);
    update(NULL, 1.0f / 120.0f);
    ik_gains_for(1.0f / 120.0f, &on, &off);
    assert(gain(image, 0) == on && gain(image, 1) == off);
    update(NULL, 1.0f / 30.0f);
    assert(near(gain(image, 0), 0.1f) && near(gain(image, 1), 0.6f));
    /* Out-of-range frame times leave the last gains; dt 0 pauses the correction. */
    update(NULL, NAN);
    update(NULL, 1.0f);
    update(NULL, -0.01f);
    assert(near(gain(image, 0), 0.1f) && near(gain(image, 1), 0.6f));
    update(NULL, 0.0f);
    assert(gain(image, 0) == 0.0f && gain(image, 1) == 1.0f);

    /* One site differs: nothing is written. */
    image = fresh();
    image[0x1515199] ^= 1;
    assert(!ik_install(image, SIZE));
    assert(!memcmp(image + 0x1514DA0, entry, 10) && near(gain(image, 0), 0.1f));
    image = fresh();
    image[0x5127B2C] ^= 1;
    assert(!ik_install(image, SIZE));
    assert(!memcmp(image + 0x1514DA0, entry, 10));

    /* BB_IK_HEIGHT=0: both gains 0, no hook. vanilla: untouched. */
    setenv("BB_IK_HEIGHT", "0", 1);
    image = fresh();
    assert(!ik_install(image, SIZE));
    assert(gain(image, 0) == 0.0f && gain(image, 1) == 0.0f && !memcmp(image + 0x1514DA0, entry, 10));
    setenv("BB_IK_HEIGHT", "off", 1);
    image = fresh();
    assert(!ik_install(image, SIZE));
    assert(gain(image, 0) == 0.0f && gain(image, 1) == 0.0f);
    setenv("BB_IK_HEIGHT", "vanilla", 1);
    image = fresh();
    assert(!ik_install(image, SIZE));
    assert(near(gain(image, 0), 0.1f) && near(gain(image, 1), 0.6f) && !memcmp(image + 0x1514DA0, entry, 10));
    unsetenv("BB_IK_HEIGHT");
    return 0;
}
