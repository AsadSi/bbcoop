/* bbcoop: the third-person camera at uncapped frame rates (runtime_camera.c, docs/camera_re.md). */
#ifndef BB_RUNTIME_CAMERA_H
#define BB_RUNTIME_CAMERA_H
#include <stdint.h>

#define CAMERA_RATE_FIELDS 5

/* Offsets of the per-frame chase fractions in ChrExFollowCam. */
extern const unsigned camera_rate_fields[CAMERA_RATE_FIELDS];

/* Rescales the camera's chase fractions from 30 FPS to the frame time dt (seconds). */
void camera_rates_apply(unsigned char *camera, float dt);

/* Installs the camera fixes (BB_CAMERA_ACCEL, BB_CAMERA_RATES). NULL on success, including "off"
 * when a site does not match Bloodborne 1.09; else a hook error. After relocation, while the image
 * is still writable. */
const char *camera_install(unsigned char *image, uint64_t size);
#endif
