/* Seamless co-op byte patches for 1.09 (EU/US, one image), from shadp2p's InstallSeamlessCoopPatches
 * (src/core/bloodborne_re.cpp, GPL-2.0-or-later). Offsets are image offsets (XML address -
 * 0x400000). Every site's original bytes are checked before anything is written, so a different
 * build or an overlapping XML patch fails as a whole instead of corrupting the game. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *name;
    uint64_t offset;
    unsigned length;
    unsigned char original[16], patched[16];
} SeamlessPatch;

static const SeamlessPatch patches[] = {
    {"Beckoning negative area (signed compare)", 0x0157F8C8, 6,
     {0x0F,0x87,0x90,0x00,0x00,0x00}, {0x0F,0x8F,0x90,0x00,0x00,0x00}},
    {"Beckoning area flag result", 0x0157F960, 2, {0x34,0x01}, {0x31,0xC0}},
    {"Responder bell area result", 0x0157F6D1, 2, {0x88,0xC3}, {0xB1,0x01}},
    {"Responder bell common result", 0x0157F6D3, 8,
     {0x4C,0x89,0xF7,0xE8,0xF5,0x9F,0x34,0x00}, {0xE9,0xE9,0x09,0x00,0x00,0x90,0x90,0x90}},
    {"Active bell negative area", 0x015068BB, 2, {0x77,0x4A}, {0x7F,0x4A}},
    {"Active bell area flag result", 0x015068FC, 5,
     {0x88,0xC3,0x80,0xF3,0x01}, {0xB3,0x01,0x90,0x90,0x90}},
    {"Responder search negative area", 0x0191A8C3, 6,
     {0x18,0xC9,0x20,0xC1,0xEB,0x02}, {0x7D,0x04,0x88,0xC1,0xEB,0x02}},
    {"SOS status area restriction", 0x018700D3, 16,
     {0x84,0xC0,0x41,0xBD,0xFF,0xFF,0xFF,0xFF,0x44,0x0F,0x44,0xEB,0x41,0xC1,0xED,0x1F},
     {0x45,0x31,0xED,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90}},
    {"Summon candidate cross-map", 0x014B714A, 6,
     {0x0F,0x85,0xC0,0x03,0x00,0x00}, {0x90,0x90,0x90,0x90,0x90,0x90}},
    {"Summon build cross-map", 0x01874B79, 2, {0x74,0x0D}, {0xEB,0x0D}},
    {"Summon build defeated-boss world state", 0x018749E8, 6,
     {0x0F,0x85,0x48,0x01,0x00,0x00}, {0x90,0x90,0x90,0x90,0x90,0x90}},
    {"Summon build negative event", 0x018749F0, 6,
     {0x0F,0x88,0x40,0x01,0x00,0x00}, {0x90,0x90,0x90,0x90,0x90,0x90}},
    /* The stage-transition destructor stops matching on a map reload, which drops a cross-map
     * guest; the call is redirected to the existence check (shadp2p bloodborne_re.cpp:3932). */
    {"Stage transition keeps matching", 0x019471B1, 5, {0xE8,0xEA,0x95,0x58,0x00}, {0xE8,0xEA,0x8E,0x58,0x00}},
};

/* Returns NULL on success, or a message naming the first site whose bytes do not match. */
const char *seamless_apply(unsigned char *image, uint64_t size) {
    static char error[160];
    size_t count = sizeof(patches) / sizeof(patches[0]);
    for (size_t i = 0; i < count; ++i) {
        const SeamlessPatch *p = &patches[i];
        if (p->offset > size || p->length > size - p->offset || memcmp(image + p->offset, p->original, p->length)) {
            snprintf(error, sizeof(error), "seamless patch \"%s\" at %#llx does not match Bloodborne 1.09",
                     p->name, (unsigned long long)p->offset);
            return error;
        }
    }
    for (size_t i = 0; i < count; ++i) memcpy(image + patches[i].offset, patches[i].patched, patches[i].length);
    printf("Seamless co-op: %zu patches applied\n", count);
    return NULL;
}
