/* bbcoop: runtime_padmotion.c's orientation maths: turning at a constant rate adds up to that angle about
 * that axis, the result stays a unit quaternion, and BB_PAD_MOTION=0 / no gamepad leave the fields at rest. */
#define _GNU_SOURCE
#include "runtime_padmotion.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>

static int near(float a, float b) { return fabsf(a - b) < 1e-3f; }

int main(void) {
    /* 90 degrees about y in 120 steps of 1/120 s at pi/2 rad/s: (0, sin 45, 0, cos 45). */
    float q[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const float w[3] = {0.0f, (float)M_PI / 2.0f, 0.0f};
    for (int i = 0; i < 120; ++i) padmotion_integrate(q, w, 1.0f / 120.0f);
    assert(near(q[0], 0.0f) && near(q[1], sqrtf(0.5f)) && near(q[2], 0.0f) && near(q[3], sqrtf(0.5f)));
    /* Any turns keep |q| = 1; no turn changes nothing. */
    const float spin[3] = {3.0f, -2.0f, 5.0f}, none[3] = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 1000; ++i) padmotion_integrate(q, spin, 1.0f / 240.0f);
    assert(near(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3], 1.0f));
    float before[4] = {q[0], q[1], q[2], q[3]};
    padmotion_integrate(q, none, 0.5f);
    for (int i = 0; i < 4; ++i) assert(near(q[i], before[i]));

    /* Without a gamepad, or with BB_PAD_MOTION=0, the fields keep their resting values. */
    float o[4] = {0.0f, 0.0f, 0.0f, 1.0f}, a[3] = {0.0f, 0.0f, 0.0f}, g[3] = {0.0f, 0.0f, 0.0f};
    setenv("BB_PAD_MOTION", "0", 1);
    padmotion_sample(NULL, o, a, g, 1000);
    assert(o[3] == 1.0f && a[0] == 0.0f && g[0] == 0.0f);
    return 0;
}
