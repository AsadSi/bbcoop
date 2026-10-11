/* bbcoop: the characters' foot-IK height correction at uncapped frame rates (1.09, EU/US, one image).
 * Models dipped into the ground for a moment when they started a new animation (the Doll at Level Up,
 * the messengers; Asad, 2026-10-11); docs/animation_re.md has the reverse engineering.
 *
 * ChrCtrl::Update (0x1514da0, rdi = ChrCtrl, xmm0 = frame time) moves the drawn model up or down by a
 * height correction (+0x260, clamped to +-0.5 m) that follows the feet's IK error: each frame
 * err += 0.1 * error while a foot is on the ground, else err *= 0.6. The gains are .data globals
 * (0x5127b28, 0x5127b2c) tuned for 30 FPS, with no frame time, so at 120 FPS the correction reacts 4x
 * faster. A hook on the update entry sets them for that call's frame time:
 *   on = 1 - (1 - 0.1)^(dt * 30), off = 0.6^(dt * 30), the game's own gains at 30 FPS.
 * They are computed from the game's constants each call, with no shared state, so character updates on
 * several threads stay correct (a character with another time scale may take another's gains for a frame).
 *   BB_IK_HEIGHT=0 (off)  no height correction at all (gains 0; feet may float on slopes): a diagnostic
 *   BB_IK_HEIGHT=vanilla  the game's per-frame gains
 * Every site's bytes are checked before anything is written; any mismatch leaves the game vanilla. */
#define _GNU_SOURCE
#include "hooks.h"
#include "runtime_ik.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHR_CTRL_UPDATE 0x1514da0
#define IK_GAIN_ON      0x5127b28
#define IK_GAIN_OFF     0x5127b2c

typedef struct {
    const char *name;
    uint64_t offset;
    unsigned length;
    unsigned char original[16];
} IkSite;

/* push rbp; mov rbp, rsp; push r15; push r14; push r13 */
static const unsigned char update_entry[10] = {0x55,0x48,0x89,0xE5,0x41,0x57,0x41,0x56,0x41,0x55};

static const IkSite sites[] = {
    {"ChrCtrl update entry", CHR_CTRL_UPDATE, 10, {0x55,0x48,0x89,0xE5,0x41,0x57,0x41,0x56,0x41,0x55}},
    {"height correction ON gain (0.1f)", IK_GAIN_ON, 4, {0xCD,0xCC,0xCC,0x3D}},
    {"height correction OFF gain (0.6f)", IK_GAIN_OFF, 4, {0x9A,0x99,0x19,0x3F}},
    /* Their only readers: vmulss xmm0, xmm2, [rip -> ON]; vmovss xmm0, [rip -> OFF]. */
    {"height correction ON step", 0x1515182, 8, {0xC5,0xEA,0x59,0x05,0x9E,0x29,0xC1,0x03}},
    {"height correction OFF step", 0x1515195, 8, {0xC5,0xFA,0x10,0x05,0x8F,0x29,0xC1,0x03}},
};

static float *gains; /* ON, then OFF, in the image */

void ik_gains_for(float dt, float *on, float *off) {
    float frames = dt * 30.0f;
    *on = 1.0f - powf(1.0f - IK_GAIN_ON_30, frames);
    *off = powf(IK_GAIN_OFF_30, frames);
}

static void chr_ctrl_update(HookRegs *regs) {
    float dt = hook_saved_xmm(regs, 0)[0];
    /* dt 0 (a character in hit-stop or frozen by its time module): on 0, off 1, the correction pauses. */
    if (!(dt >= 0.0f && dt < 0.25f)) return; /* no usable frame time: the last gains */
    float on, off;
    ik_gains_for(dt, &on, &off);
    __atomic_store(&gains[0], &on, __ATOMIC_RELAXED);
    __atomic_store(&gains[1], &off, __ATOMIC_RELAXED);
}

const char *ik_install(unsigned char *image, uint64_t size) {
    const char *env = getenv("BB_IK_HEIGHT");
    int vanilla = env && !strcmp(env, "vanilla");
    int none = env && (!strcmp(env, "0") || !strcmp(env, "off") || !strcmp(env, "false"));
    if (vanilla) {
        printf("IK height: vanilla (BB_IK_HEIGHT=vanilla)\n");
        return NULL;
    }
    for (size_t i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i) {
        const IkSite *s = &sites[i];
        if (s->offset > size || s->length > size - s->offset || memcmp(image + s->offset, s->original, s->length)) {
            printf("IK height: off: site \"%s\" at %#llx does not match Bloodborne 1.09\n", s->name,
                   (unsigned long long)s->offset);
            return NULL;
        }
    }
    gains = (float *)(void *)(image + IK_GAIN_ON);
    if (none) {
        static const float zero[2] = {0.0f, 0.0f};
        memcpy(gains, zero, sizeof(zero));
        printf("IK height: correction off (BB_IK_HEIGHT=0)\n");
        return NULL;
    }
    /* Nothing is written when the stub area or the hook fails. Only a failed seal, after the jump is in
     * place, stops the game. */
    const char *failure = hooks_init(image, size);
    if (!failure) failure = hooks_install(CHR_CTRL_UPDATE, update_entry, sizeof(update_entry), chr_ctrl_update);
    if (failure) {
        printf("IK height: off: %s\n", failure);
        return NULL;
    }
    if ((failure = hooks_seal())) return failure;
    printf("IK height: correction gains follow the frame time\n");
    return NULL;
}
