/* bbcoop: Duplicate in the Load Game list (runtime_saveslots.c, docs/saveslots_re.md). */
#ifndef BB_SAVESLOTS_H
#define BB_SAVESLOTS_H
#include <stdint.h>

/* Installs the hooks; text_hook: the menu text hook (runtime_respec.c) is installed and calls
 * saveslots_text, without which Duplicate stays off. NULL or an error. */
const char *saveslots_install(unsigned char *image, uint64_t size, int text_hook);
/* From the menu text hook: swaps the Delete confirm for the Duplicate one while ours runs; 1 if swapped. */
int saveslots_text(uint32_t fmg, uint32_t *id);
/* The copy's name: `source` without a " (n)" suffix, plus " (n)" with the smallest n >= 2 that none of
 * `taken` has, cut to 16 UTF-16 characters (zero-filled to 17). */
void saveslots_copy_name(const uint16_t *source, const uint16_t taken[][17], int count, uint16_t out[17]);
#endif
