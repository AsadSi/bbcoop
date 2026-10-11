/* bbcoop: runtime_messenger.c on a synthetic image: off unless BB_MESSENGER_RADIUS is set, the byte
 * check, the range check and the written radius. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

const char *messenger_install(unsigned char *image, uint64_t size);

#define SIZE 0x04A00000
#define SITE 0x4928b50

static float radius_squared(const unsigned char *image) {
    float value;
    memcpy(&value, image + SITE, 4);
    return value;
}

int main(void) {
    unsigned char *image = calloc(1, SIZE);
    assert(image);
    const float vanilla = 25.0f;
    memcpy(image + SITE, &vanilla, 4);

    unsetenv("BB_MESSENGER_RADIUS");
    assert(!messenger_install(image, SIZE) && radius_squared(image) == 25.0f);
    setenv("BB_MESSENGER_RADIUS", "200", 1);
    assert(!messenger_install(image, SIZE) && radius_squared(image) == 25.0f);
    setenv("BB_MESSENGER_RADIUS", "5m", 1);
    assert(!messenger_install(image, SIZE) && radius_squared(image) == 25.0f);
    setenv("BB_MESSENGER_RADIUS", "20", 1);
    assert(!messenger_install(image, SIZE) && radius_squared(image) == 400.0f);
    /* Already changed: the byte check fails and nothing more is written. */
    setenv("BB_MESSENGER_RADIUS", "10", 1);
    assert(!messenger_install(image, SIZE) && radius_squared(image) == 400.0f);
    assert(!messenger_install(image, SITE + 2));
    free(image);
    return 0;
}
