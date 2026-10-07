#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

const char *seamless_apply(unsigned char *image, uint64_t size);

#define SIZE 0x01900000
static void put(unsigned char *image, uint64_t offset, const unsigned char *bytes, size_t length) {
    memcpy(image + offset, bytes, length);
}

/* The original 01.09 bytes at every site (runtime_seamless.c). */
static void original(unsigned char *image) {
    put(image, 0x0157F8C8, (const unsigned char[]){0x0F,0x87,0x90,0x00,0x00,0x00}, 6);
    put(image, 0x0157F960, (const unsigned char[]){0x34,0x01}, 2);
    put(image, 0x0157F6D1, (const unsigned char[]){0x88,0xC3}, 2);
    put(image, 0x0157F6D3, (const unsigned char[]){0x4C,0x89,0xF7,0xE8,0xF5,0x9F,0x34,0x00}, 8);
    put(image, 0x015068BB, (const unsigned char[]){0x77,0x4A}, 2);
    put(image, 0x015068FC, (const unsigned char[]){0x88,0xC3,0x80,0xF3,0x01}, 5);
    put(image, 0x0191A8C3, (const unsigned char[]){0x18,0xC9,0x20,0xC1,0xEB,0x02}, 6);
    put(image, 0x018700D3, (const unsigned char[]){0x84,0xC0,0x41,0xBD,0xFF,0xFF,0xFF,0xFF,0x44,0x0F,0x44,0xEB,0x41,0xC1,0xED,0x1F}, 16);
    put(image, 0x014B714A, (const unsigned char[]){0x0F,0x85,0xC0,0x03,0x00,0x00}, 6);
    put(image, 0x01874B79, (const unsigned char[]){0x74,0x0D}, 2);
    put(image, 0x018749E8, (const unsigned char[]){0x0F,0x85,0x48,0x01,0x00,0x00}, 6);
    put(image, 0x018749F0, (const unsigned char[]){0x0F,0x88,0x40,0x01,0x00,0x00}, 6);
    put(image, 0x019471B1, (const unsigned char[]){0xE8,0xEA,0x95,0x58,0x00}, 5);
}

int main(void) {
    unsigned char *image = calloc(1, 0x01A00000), *copy = malloc(0x01A00000);
    assert(image && copy);

    /* A blank (wrong) build: refused, nothing written. */
    assert(seamless_apply(image, 0x01A00000) != NULL);
    for (size_t i = 0; i < 0x01A00000; ++i) assert(image[i] == 0);

    /* One wrong site among correct ones: refused, still nothing written. */
    original(image);
    image[0x018749F0] = 0xCC;
    memcpy(copy, image, 0x01A00000);
    const char *error = seamless_apply(image, 0x01A00000);
    assert(error && strstr(error, "negative event"));
    assert(!memcmp(copy, image, 0x01A00000));

    /* An image too small to hold a site: refused. */
    image[0x018749F0] = 0x0F;
    assert(seamless_apply(image, SIZE) != NULL);

    /* The real bytes: every site patched. */
    assert(seamless_apply(image, 0x01A00000) == NULL);
    assert(image[0x0157F8C9] == 0x8F && image[0x01874B79] == 0xEB && image[0x018700D3] == 0x45);
    assert(!memcmp(image + 0x014B714A, (const unsigned char[]){0x90,0x90,0x90,0x90,0x90,0x90}, 6));
    assert(!memcmp(image + 0x019471B1, (const unsigned char[]){0xE8,0xEA,0x8E,0x58,0x00}, 5));

    /* Applying twice is refused: the sites no longer hold the originals. */
    assert(seamless_apply(image, 0x01A00000) != NULL);
    free(image);
    free(copy);
    return 0;
}
