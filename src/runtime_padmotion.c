/* bbcoop: the DualShock's motion sensors for the game (docs/padmotion_re.md). The port's scePadReadState
 * always said "at rest" (no acceleration, no rotation, identity orientation); this fills the three
 * fields from SDL's gamepad sensors, as a PS4 pad does:
 *   acceleration      in g (SDL gives m/s^2): the game clamps it to +-3 and scales it to +-1 (0x2690444)
 *   angular_velocity  in rad/s, as SDL gives it: the game clamps it to +-6.53
 *   orientation       a quaternion (x, y, z, w): the angular velocity added up since scePadResetOrientation
 * Axes are SDL's for a pad held in front of you (x right, y up, z toward you), taken as the PS4's.
 *   BB_PAD_MOTION=0        the fields stay at rest (no motion controls)
 *   BB_PAD_MOTION_TRACE=1  logs the sensor values once a second while the pad moves */
#include "runtime_padmotion.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_STEP_S 0.1f /* longer gaps (a pause, a reconnect) do not turn the orientation */

static int enabled = -1, trace;
static SDL_Gamepad *sensed;        /* the gamepad whose sensors are on */
static int has_accel, has_gyro;
static float orientation_now[4] = {0.0f, 0.0f, 0.0f, 1.0f};
static uint64_t last_us, last_trace_us;
static int reset_requested;

void padmotion_reset_orientation(void) { __atomic_store_n(&reset_requested, 1, __ATOMIC_RELAXED); }

void padmotion_integrate(float q[4], const float w[3], float dt) {
    /* q' = q + dt/2 * q * (w, 0), then normalised. */
    const float x = q[0], y = q[1], z = q[2], s = q[3], h = 0.5f * dt;
    q[0] = x + h * (s * w[0] + y * w[2] - z * w[1]);
    q[1] = y + h * (s * w[1] + z * w[0] - x * w[2]);
    q[2] = z + h * (s * w[2] + x * w[1] - y * w[0]);
    q[3] = s - h * (x * w[0] + y * w[1] + z * w[2]);
    const float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(n > 0.0f)) { q[0] = q[1] = q[2] = 0.0f; q[3] = 1.0f; return; }
    for (int i = 0; i < 4; ++i) q[i] /= n;
}

static void enable_sensors(SDL_Gamepad *g) {
    sensed = g;
    has_accel = SDL_GamepadHasSensor(g, SDL_SENSOR_ACCEL) && SDL_SetGamepadSensorEnabled(g, SDL_SENSOR_ACCEL, true);
    has_gyro = SDL_GamepadHasSensor(g, SDL_SENSOR_GYRO) && SDL_SetGamepadSensorEnabled(g, SDL_SENSOR_GYRO, true);
    last_us = 0;
    padmotion_reset_orientation();
    printf("Runtime: gamepad motion: %s%s%s\n", has_accel ? "accelerometer" : "no accelerometer",
           has_gyro ? ", gyro" : ", no gyro", has_accel || has_gyro ? " (BB_PAD_MOTION=0 turns it off)" : "");
}

void padmotion_sample(SDL_Gamepad *g, float orientation[4], float acceleration[3], float angular_velocity[3],
                      uint64_t now_us) {
    if (enabled < 0) {
        const char *env = getenv("BB_PAD_MOTION");
        enabled = !(env && (!strcmp(env, "0") || !strcmp(env, "off") || !strcmp(env, "false")));
        trace = (env = getenv("BB_PAD_MOTION_TRACE")) && !strcmp(env, "1");
        if (!enabled) puts("Runtime: gamepad motion off (BB_PAD_MOTION=0)");
    }
    if (!enabled || !g) return;
    if (g != sensed) enable_sensors(g);
    float a[3] = {0.0f, 0.0f, 0.0f}, w[3] = {0.0f, 0.0f, 0.0f};
    if (has_accel && SDL_GetGamepadSensorData(g, SDL_SENSOR_ACCEL, a, 3))
        for (int i = 0; i < 3; ++i) acceleration[i] = a[i] / SDL_STANDARD_GRAVITY;
    if (has_gyro && SDL_GetGamepadSensorData(g, SDL_SENSOR_GYRO, w, 3)) {
        memcpy(angular_velocity, w, sizeof(w));
        if (__atomic_exchange_n(&reset_requested, 0, __ATOMIC_RELAXED)) {
            orientation_now[0] = orientation_now[1] = orientation_now[2] = 0.0f;
            orientation_now[3] = 1.0f;
        }
        const float dt = last_us && now_us > last_us ? (float)(now_us - last_us) / 1e6f : 0.0f;
        if (dt > 0.0f && dt <= MAX_STEP_S) padmotion_integrate(orientation_now, w, dt);
        last_us = now_us;
        memcpy(orientation, orientation_now, sizeof(orientation_now));
    }
    if (trace && now_us - last_trace_us >= 1000000 && fabsf(w[0]) + fabsf(w[1]) + fabsf(w[2]) > 0.5f) {
        last_trace_us = now_us;
        printf("Runtime: motion accel %.2f %.2f %.2f g, gyro %.2f %.2f %.2f rad/s, orientation %.2f %.2f %.2f %.2f\n",
               acceleration[0], acceleration[1], acceleration[2], w[0], w[1], w[2],
               orientation[0], orientation[1], orientation[2], orientation[3]);
    }
}
