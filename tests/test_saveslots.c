/* bbcoop: runtime_saveslots.c's naming of a duplicated profile. */
#define _GNU_SOURCE
#include "runtime_saveslots.h"
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

__thread sigjmp_buf *runtime_fault_recover;
int runtime_savedata_last_host(const char *dir, char *out, size_t size) { (void)dir; (void)out; (void)size; return 0; }

static void wide(const char *s, uint16_t out[17]) {
    memset(out, 0, 17 * sizeof(uint16_t));
    for (int i = 0; s[i] && i < 16; ++i) out[i] = (uint16_t)s[i];
}

static int check(const char *source, const char *const *taken, int count, const char *want) {
    uint16_t src[17], names[10][17], out[17], expected[17];
    wide(source, src);
    for (int i = 0; i < count; ++i) wide(taken[i], names[i]);
    wide(want, expected);
    saveslots_copy_name(src, (const uint16_t (*)[17])names, count, out);
    if (!memcmp(out, expected, sizeof(out))) return 0;
    char got[18] = {0};
    for (int i = 0; i < 17; ++i) got[i] = (char)out[i];
    printf("FAIL: copy of \"%s\": \"%s\", expected \"%s\"\n", source, got, want);
    return 1;
}

int main(void) {
    int failed = 0;
    const char *one[] = {"Hoonter", "TEST"};
    failed += check("Hoonter", one, 2, "Hoonter (2)");
    const char *two[] = {"Hoonter", "TEST", "Hoonter (2)"};
    failed += check("Hoonter", two, 3, "Hoonter (3)");
    failed += check("Hoonter (2)", two, 3, "Hoonter (3)");      /* a copy of a copy: the next number */
    const char *gap[] = {"Hoonter", "Hoonter (3)"};
    failed += check("Hoonter", gap, 2, "Hoonter (2)");          /* the smallest free number */
    const char *full[] = {"ABCDEFGHIJKLMNOP"};
    failed += check("ABCDEFGHIJKLMNOP", full, 1, "ABCDEFGHIJKL (2)"); /* cut to 16 */
    const char *paren[] = {"Mr (x)"};
    failed += check("Mr (x)", paren, 1, "Mr (x) (2)");          /* not our suffix: kept */
    const char *none[] = {""};
    failed += check("", none, 0, " (2)");
    if (failed) return 1;
    printf("saveslots: copy names ok\n");
    return 0;
}
