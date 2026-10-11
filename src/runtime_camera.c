/* bbcoop: the third-person camera at uncapped frame rates (1.09, EU/US, one image). The camera
 * sped up at times (Asad, 2026-10-10); docs/camera_re.md has the reverse engineering.
 *
 * - Turn ramp: holding the stick fully for 0.6 s (RotHiSpeedRateIncTime) ramps the camera from
 *   ~120 to ~260 degrees/s; the ramp is vanilla behaviour. It is set to 1e9 s, so turning keeps one
 *   speed. BB_CAMERA_ACCEL=1 keeps the vanilla ramp.
 * - Chase rates: lock-on, pitch return and lock-adjust move a fixed fraction of the way per frame,
 *   tuned for 30 FPS, so at 120 FPS they are about 4x snappier. A hook on the camera update
 *   (0x143ac60, rdi = camera, xmm0 = frame time) rescales each fraction to the frame time:
 *   rate' = 1 - (1 - rate)^(dt * 30), which equals the game's own rate at 30 FPS.
 *   BB_CAMERA_RATES=0 turns it off; run.sh does for the fixed 30/60/90 FPS modes, whose patches tune
 *   the rates themselves.
 * Every site's bytes are checked before anything is written; any mismatch leaves the camera vanilla. */
#define _GNU_SOURCE
#include "hooks.h"
#include "runtime_camera.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAMERA_UPDATE 0x143ac60
#define CAMERA_SLOTS 8

typedef struct {
    const char *name;
    uint64_t offset;
    unsigned length;
    unsigned char original[16], patched[16];
} CameraPatch;

/* mov dword [rbx+0x1e8], imm32 in both ChrExFollowCam init blocks: 0.6f becomes 1e9f. */
static const CameraPatch ramp_patches[] = {
    {"Follow camera turn ramp time", 0x0143A57B, 10,
     {0xC7,0x83,0xE8,0x01,0x00,0x00,0x9A,0x99,0x19,0x3F}, {0xC7,0x83,0xE8,0x01,0x00,0x00,0x28,0x6B,0x6E,0x4E}},
    {"Follow camera turn ramp time (second init)", 0x0143A979, 10,
     {0xC7,0x83,0xE8,0x01,0x00,0x00,0x9A,0x99,0x19,0x3F}, {0xC7,0x83,0xE8,0x01,0x00,0x00,0x28,0x6B,0x6E,0x4E}},
};

/* push rbp; mov rbp, rsp; push r15; push r14; push r13 */
static const unsigned char update_entry[10] = {0x55,0x48,0x89,0xE5,0x41,0x57,0x41,0x56,0x41,0x55};

/* Per-frame lerp fractions of ChrExFollowCam and where the update reads them. */
const unsigned camera_rate_fields[CAMERA_RATE_FIELDS] = {
    0x240, /* LockRotChaseRate, 0.3 (0x143c30f, 0x143c784) */
    0x1a8, /* TargetRotXChaseRate, 0.3 (0x143cef0) */
    0x23c, /* DirectPitchRate, 0.06 (0x143f97e, 0x143fa9f) */
    0x288, /* lock-adjust rate, 0.3 (0x143c9e6) */
    0x330, /* LockCamParam interpolation, 0.05 (0x143ae75) */
};

/* "Uncap FPS++" (patches/Bloodborne.xml, at 0x1440799) halves DirectPitchRate to 0.03 for ~60 FPS;
 * the hook's base is the 30 FPS rate, so that value stands for the vanilla 0.06. */
static const struct { unsigned field; uint32_t patched, vanilla; } patched_bases[] = {
    {2, 0x3CF5C28F, 0x3D75C28F},
};

typedef struct {
    uintptr_t camera;
    uint64_t used;
    float base[CAMERA_RATE_FIELDS], written[CAMERA_RATE_FIELDS];
} CameraSlot;

/* The camera update runs on the game thread only (its two callers, 0x1436c00 and 0x1436e15). */
static CameraSlot slots[CAMERA_SLOTS];
static uint64_t frames;

static CameraSlot *slot_for(uintptr_t camera) {
    CameraSlot *slot = &slots[0];
    for (unsigned i = 0; i < CAMERA_SLOTS; ++i) {
        if (slots[i].camera == camera) { slots[i].used = ++frames; return &slots[i]; }
        if (slots[i].used < slot->used) slot = &slots[i]; /* least recently used */
    }
    slot->camera = camera;
    slot->used = ++frames;
    /* NaN never equals a field, so every field is taken as the game's own value on first sight. */
    for (unsigned i = 0; i < CAMERA_RATE_FIELDS; ++i) slot->written[i] = NAN;
    return slot;
}

void camera_rates_apply(unsigned char *camera, float dt) {
    if (!(dt > 0.0005f && dt < 0.25f)) return; /* no frame time worth rescaling for */
    CameraSlot *slot = slot_for((uintptr_t)camera);
    for (unsigned i = 0; i < CAMERA_RATE_FIELDS; ++i) {
        float value;
        memcpy(&value, camera + camera_rate_fields[i], 4);
        /* A value other than the one written last is the game's (initialised or reloaded). */
        if (value != slot->written[i]) {
            slot->base[i] = value;
            for (unsigned k = 0; k < sizeof(patched_bases) / sizeof(patched_bases[0]); ++k) {
                uint32_t bits;
                memcpy(&bits, &value, 4);
                if (patched_bases[k].field == i && patched_bases[k].patched == bits)
                    memcpy(&slot->base[i], &patched_bases[k].vanilla, 4);
            }
        }
        float base = slot->base[i];
        float rate = base > 0.0f && base < 1.0f ? 1.0f - powf(1.0f - base, dt * 30.0f) : base;
        memcpy(camera + camera_rate_fields[i], &rate, 4);
        slot->written[i] = rate;
    }
}

static void camera_update(HookRegs *regs) {
    float dt = hook_saved_xmm(regs, 0)[0];
    if (regs->rdi) camera_rates_apply((unsigned char *)(uintptr_t)regs->rdi, dt);
}

static int flag(const char *name, int fallback) {
    const char *env = getenv(name);
    return env && *env ? strcmp(env, "0") != 0 : fallback;
}

const char *camera_install(unsigned char *image, uint64_t size) {
    int ramp = !flag("BB_CAMERA_ACCEL", 0), rates = flag("BB_CAMERA_RATES", 1);
    if (!ramp && !rates) return NULL;
    size_t count = sizeof(ramp_patches) / sizeof(ramp_patches[0]);
    for (size_t i = 0; i < count; ++i) {
        const CameraPatch *p = &ramp_patches[i];
        if (p->offset > size || p->length > size - p->offset || memcmp(image + p->offset, p->original, p->length)) {
            printf("Camera: off: site \"%s\" at %#llx does not match Bloodborne 1.09\n", p->name,
                   (unsigned long long)p->offset);
            return NULL;
        }
    }
    if (CAMERA_UPDATE + sizeof(update_entry) > size || memcmp(image + CAMERA_UPDATE, update_entry, sizeof(update_entry))) {
        printf("Camera: off: the camera update at %#x does not match Bloodborne 1.09\n", CAMERA_UPDATE);
        return NULL;
    }
    if (rates) {
        /* Nothing is written when the stub area or the hook fails: the camera stays vanilla. Only a
         * failed seal, after the jump is in place, stops the game. */
        const char *failure = hooks_init(image, size);
        if (!failure) failure = hooks_install(CAMERA_UPDATE, update_entry, sizeof(update_entry), camera_update);
        if (failure) {
            printf("Camera: off: %s\n", failure);
            return NULL;
        }
        if ((failure = hooks_seal())) return failure;
    }
    if (ramp)
        for (size_t i = 0; i < count; ++i) memcpy(image + ramp_patches[i].offset, ramp_patches[i].patched, ramp_patches[i].length);
    printf("Camera: %s%s%s\n", ramp ? "turn ramp off" : "", ramp && rates ? ", " : "",
           rates ? "chase rates follow the frame time" : "");
    return NULL;
}
