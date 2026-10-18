/* bbcoop: the DualShock's motion sensors for scePadReadState (runtime_padmotion.c, docs/padmotion_re.md). */
#ifndef BB_RUNTIME_PADMOTION_H
#define BB_RUNTIME_PADMOTION_H
#include <stdint.h>

typedef struct SDL_Gamepad SDL_Gamepad;

/* Fills the pad data's motion fields from the gamepad's sensors: acceleration in g, angular velocity in
 * rad/s, and the orientation the angular velocity adds up to since the last reset. Leaves them as they are
 * (at rest) for a gamepad without sensors, or with BB_PAD_MOTION=0. Call under the pad lock. */
void padmotion_sample(SDL_Gamepad *g, float orientation[4], float acceleration[3], float angular_velocity[3],
                      uint64_t now_us);

/* scePadResetOrientation: the next sample starts again from the identity orientation. */
void padmotion_reset_orientation(void);

/* One step of the orientation: q (x, y, z, w) turned by the angular velocity w (rad/s) for dt seconds. */
void padmotion_integrate(float q[4], const float w[3], float dt);
#endif
