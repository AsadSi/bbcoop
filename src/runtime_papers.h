/* bbcoop: Fire Paper, Bolt Paper and Empty Phantasm Shell scale with the attack (runtime_papers.c,
 * docs/papers_re.md). */
#ifndef BB_RUNTIME_PAPERS_H
#define BB_RUNTIME_PAPERS_H
#include <stdint.h>

#define PAPERS_REFERENCE_BASE 90.0f /* Saw Cleaver +0: its R1 (multiplier 100%) keeps the item's own +80 */
#define PAPERS_SHARE          0.1f  /* of the weapon's base at its current level x the attack's multiplier */

/* The bonus that replaces the item's flat `value` (80) for one hit: value x (base at +0 x multiplier) / 90,
 * plus 10% x (base at the current level x multiplier). Bases are EquipParamWeapon attackBasePhysics,
 * multiplier the attack's atkPhysCorrection / 100 (the item's element's correction when that is 0). */
float papers_bonus(float value, float base0, float base_now, float multiplier);

/* Installs the hook (BB_PAPERS=vanilla, 0, off or false: off). NULL on success, including "off" when a site does not
 * match Bloodborne 1.09; else a hook error. After relocation, while the image is still writable. */
const char *papers_install(unsigned char *image, uint64_t size);
#endif
