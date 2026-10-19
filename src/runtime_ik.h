/* bbcoop: the characters' foot-IK height correction at uncapped frame rates (runtime_ik.c,
 * docs/bbcoop/animation_re.md). */
#ifndef BB_RUNTIME_IK_H
#define BB_RUNTIME_IK_H
#include <stdint.h>

/* The game's per-frame gains (30 FPS): err += on * error with a foot on the ground, else err *= off. */
#define IK_GAIN_ON_30  0.1f
#define IK_GAIN_OFF_30 0.6f

/* The gains for a frame time dt (seconds), same response over time as the game's at 30 FPS. */
void ik_gains_for(float dt, float *on, float *off);

/* Installs the fix (BB_IK_HEIGHT). NULL on success, including "off" when a site does not match
 * Bloodborne 1.09; else a hook error. After relocation, while the image is still writable. */
const char *ik_install(unsigned char *image, uint64_t size);
#endif
