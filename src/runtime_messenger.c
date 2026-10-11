/* bbcoop: the messengers' emerge radius (1.09, EU/US, one image), a diagnostic for "messengers vanish
 * at odd distances" (Asad, 2026-10-11). The note, bloodstain and summon-sign messengers
 * (SprjObjMessenger) come out of the ground while the player is within 5 m and sink beyond it: the
 * "player is near" test (0x1cc6a30) compares the squared distance with 25.0f at 0x4928b50, read only
 * at 0x1cc6afa. BB_MESSENGER_RADIUS=<metres> (0.5 to 100) changes that radius; unset, the game keeps
 * its own. Lamp messengers follow a flag instead, so they are not affected. The site's bytes are
 * checked before anything is written. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define NEAR_RADIUS_SQUARED 0x4928b50

const char *messenger_install(unsigned char *image, uint64_t size) {
    const char *env = getenv("BB_MESSENGER_RADIUS");
    if (!env || !*env) return NULL;
    char *end;
    float radius = strtof(env, &end);
    if (*end || !(radius >= 0.5f && radius <= 100.0f)) {
        printf("Messengers: BB_MESSENGER_RADIUS=%s ignored (0.5 to 100 metres)\n", env);
        return NULL;
    }
    static const unsigned char vanilla[4] = {0x00, 0x00, 0xC8, 0x41}; /* 25.0f */
    if (NEAR_RADIUS_SQUARED + 4 > size || memcmp(image + NEAR_RADIUS_SQUARED, vanilla, 4)) {
        printf("Messengers: off: the emerge radius at %#x does not match Bloodborne 1.09\n", NEAR_RADIUS_SQUARED);
        return NULL;
    }
    float squared = radius * radius;
    memcpy(image + NEAR_RADIUS_SQUARED, &squared, 4);
    printf("Messengers: emerge within %.1f m (the game's: 5 m)\n", radius);
    return NULL;
}
