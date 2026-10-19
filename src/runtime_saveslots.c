/* bbcoop: "Duplicate" in the title screen's Load Game list (Bloodborne 1.09, EU/US, one image). Triangle
 * on a profile asks "Duplicate selected profile?"; Yes copies the character, name and all, into the first
 * free profile slot, which then shows in the list as a character of its own: loading it
 * goes on from there and autosaves into its own slot, the original keeps its own. Every copy is kept
 * until deleted in the same list (10 slots in all). docs/bbcoop/saveslots_re.md has the reverse engineering.
 *
 * The game's Delete does: a Yes/No box (204/406013), then job A (0x1b45de0: exists[slot] = 0), then the
 * list refresh and the system-data save (userdata0010: the profile summaries and their exists flags).
 * Duplicate runs that same chain with the slot tagged (+0x100), so the game's own clear does nothing, and
 * at job A (after Yes) copies the slot file on the host (the game reads a slot file from disk on every
 * load), the in-memory summary record (0x220 bytes) and sets exists[new]; the game's save then writes
 * userdata0010. With no free slot the box says so and Yes does nothing (tag +0x200).
 * Every site's bytes are checked first; any mismatch leaves the game vanilla. BB_SAVESLOTS=0: off. */
#define _GNU_SOURCE
#include "hooks.h"
#include "runtime.h"
#include "runtime_saveslots.h"
#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* runtime_savedata.c (bbcoop): the host directory last mounted for a save directory name. */
int runtime_savedata_last_host(const char *dir, char *out, size_t size);

#define KEYGUIDE_ADDED   0x01B42F86 /* after the Delete button's binding: r15 the dialog, rbx the idle predicate */
#define DELETE_SLOT      0x01B4334E /* in Delete's action (0x1b43230): r14d the selected slot, job A's capture next */
#define CLEAR_SLOT       0x01B45DED /* job A (0x1b45de0): eax the captured slot, rcx the GameDataMan */
#define TEXT_LOOKUP      0x01AE8CC0 /* (out wstring, fmg, id) */
#define KEYGUIDE_TRIANGLE 0x01AE1F30 /* (out item, const wstring *label): the Triangle button */
#define BINDING_ADD      0x01AD5730 /* (dialog, item, &action, &predicate); copies all three */
#define DELETE_ACTION    0x01B43230 /* the Delete button's action (this: {vtable, dialog}) */
#define GAME_NEW         0x02BBE9A8 /* operator new */
#define GAME_DATA_MAN_PTR 0x0553B130 /* [+0x58]: the profile summaries: exists[10] at +0x10, records at +0x20 */
#define FN_DESTROY       0x01B439B0 /* the Delete action's vtable [3..7], reused for ours */

#define KEYGUIDE_FMG     202     /* SP key guide */
#define DIALOG_FMG       204     /* MenuDialogs_SP */
#define DELETE_CONFIRM   406013  /* "OK to delete selected profile?" */
#define TEXT_DUPLICATE   400090  /* "Duplicate" (tools/respec_mod.py adds these three) */
#define TEXT_CONFIRM     406090  /* "Duplicate selected profile?" */
#define TEXT_NO_SLOT     406091  /* "No free profile slot ..." */
#define SLOTS            10
#define RECORD_SIZE      0x220
#define SLOT_FILE_SIZE   0x140000
#define NAME_CHARS       16
#define TAG_DUPLICATE    0x100
#define TAG_NO_SLOT      0x200

static uint64_t image_base;
static int duplicating; /* inside our button's call of the Delete action (game thread) */
static int installed;

typedef void (ABI *TextLookup)(void *out, int32_t fmg, int32_t id);
typedef void (ABI *KeyguideCtor)(void *item, const void *label);
typedef void (ABI *BindingAdd)(uint64_t dialog, void *item, void *action, void *predicate);
typedef void (ABI *ActionCall)(void *self);
typedef void *(ABI *GameNew)(uint64_t size);
typedef void (ABI *Destroy)(void *self, int deallocate);
typedef void (ABI *AllocFree)(void *alloc, void *ptr);
#define NATIVE(type, offset) ((type)(uintptr_t)(image_base + (offset)))

static int guest_access(uint64_t base, uint64_t offset, void *host, size_t size, int write) {
    if (base < 0x10000 || base + offset < base) return 0;
    void *guest = (void *)(uintptr_t)(base + offset);
    sigjmp_buf recover, *previous = runtime_fault_recover;
    if (sigsetjmp(recover, 0)) {
        runtime_fault_recover = previous;
        return 0;
    }
    runtime_fault_recover = &recover;
    __asm__ volatile("" ::: "memory");
    if (write) memcpy(guest, host, size);
    else memcpy(host, guest, size);
    __asm__ volatile("" ::: "memory");
    runtime_fault_recover = previous;
    return 1;
}
static uint64_t read_u64(uint64_t base, uint64_t offset) { uint64_t v = 0; guest_access(base, offset, &v, 8, 0); return v; }

/* The profile summaries (title screen, after userdata0010 was read): exists[i] at +0x10+i. */
static uint64_t summaries(void) { return read_u64(read_u64(image_base, GAME_DATA_MAN_PTR), 0x58); }

static int slot_exists(uint64_t s, int slot) {
    uint8_t on = 0;
    return guest_access(s, 0x10 + (uint64_t)slot, &on, 1, 0) && on;
}

static int free_slot(uint64_t s) {
    for (int i = 0; i < SLOTS; ++i)
        if (!slot_exists(s, i)) return i;
    return -1;
}

/* ---- the copy ---- */

/* A byte copy (the slot file stores no slot number or ID), to a temporary file first. */
static int copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    if (!in) return 0;
    unsigned char *data = malloc(SLOT_FILE_SIZE + 1);
    size_t got = data ? fread(data, 1, SLOT_FILE_SIZE + 1, in) : 0;
    fclose(in);
    if (got != SLOT_FILE_SIZE) {
        free(data);
        return 0;
    }
    char temporary[1200];
    snprintf(temporary, sizeof(temporary), "%s.tmp", to);
    FILE *out = fopen(temporary, "wb");
    int ok = out && fwrite(data, 1, SLOT_FILE_SIZE, out) == SLOT_FILE_SIZE;
    if (out) ok = !fclose(out) && ok;
    free(data);
    if (ok) ok = !rename(temporary, to);
    if (!ok) unlink(temporary);
    return ok;
}

static void duplicate(int source) {
    uint64_t s = summaries();
    int target = free_slot(s);
    if (source < 0 || source >= SLOTS || !slot_exists(s, source) || target < 0) {
        printf("Saveslots: duplicate refused: slot %d (exists %d), free slot %d\n", source,
               source >= 0 && source < SLOTS && slot_exists(s, source), target);
        return;
    }
    char dir[1024], from[1100], to[1100], backup[1100];
    if (!runtime_savedata_last_host("SPRJ0005", dir, sizeof(dir))) {
        printf("Saveslots: duplicate refused: the save directory is not known\n");
        return;
    }
    snprintf(from, sizeof(from), "%s/userdata%04d", dir, source);
    snprintf(to, sizeof(to), "%s/userdata%04d", dir, target);
    snprintf(backup, sizeof(backup), "%s/backup%04d", dir, target);
    unsigned char record[RECORD_SIZE];
    if (!guest_access(s, 0x20 + (uint64_t)RECORD_SIZE * (uint64_t)source, record, sizeof(record), 0)) return;
    if (!copy_file(from, to)) {
        printf("Saveslots: duplicate failed: could not copy %s to %s\n", from, to);
        return;
    }
    /* backupN is the game's fallback when userdataN cannot be read: an older (deleted) character. */
    unlink(backup);
    uint8_t on = 1;
    if (!guest_access(s, 0x20 + (uint64_t)RECORD_SIZE * (uint64_t)target, record, sizeof(record), 1) ||
        !guest_access(s, 0x10 + (uint64_t)target, &on, 1, 1)) {
        unlink(to);
        printf("Saveslots: duplicate failed: the profile list could not be updated\n");
        return;
    }
    char name[NAME_CHARS + 1];
    int length = 0;
    for (uint16_t c; length < NAME_CHARS && (c = (uint16_t)(record[2 * length] | record[2 * length + 1] << 8)); ++length)
        name[length] = c < 128 ? (char)c : '?';
    name[length] = 0;
    printf("Saveslots: slot %d duplicated into slot %d, same name \"%s\" (%s); the game saves the profile list next\n",
           source, target, name, to);
}

/* ---- hooks ---- */

/* Our button's action: a std::function target {vtable, dialog} like the Delete button's, whose call runs
 * the Delete action with `duplicating` set. Copies keep our vtable. */
static void *action_vtable[9];

static ABI void *action_clone(void *self, void *into) {
    if (!into) into = NATIVE(GameNew, GAME_NEW)(0x18);
    if (!into) return NULL;
    ((void **)into)[0] = &action_vtable[1];
    ((void **)into)[1] = ((void **)self)[1];
    return into;
}

static ABI void action_call(void *self) {
    duplicating = 1;
    NATIVE(ActionCall, DELETE_ACTION)(self);
    duplicating = 0;
}

static void free_wstring(unsigned char *s, unsigned buffer, unsigned capacity, unsigned allocator) {
    uint64_t cap, ptr, alloc;
    memcpy(&cap, s + capacity, 8);
    memcpy(&ptr, s + buffer, 8);
    memcpy(&alloc, s + allocator, 8);
    if (cap >= 8 && alloc) {
        AllocFree release = (AllocFree)(uintptr_t)((uint64_t *)(uintptr_t)(*(uint64_t *)(uintptr_t)alloc))[0x70 / 8];
        release((void *)(uintptr_t)alloc, (void *)(uintptr_t)ptr);
    }
}

/* After the Delete button's binding (Load Game only: the list allows Delete): a Triangle "Duplicate"
 * button with the same idle predicate (rbx, still alive). The item and the label are built as the game
 * builds the Delete button's, and freed as it frees them; the binding keeps copies. */
static int warned;
static void keyguide_added(HookRegs *regs) {
    uint64_t dialog = regs->r15;
    _Alignas(16) unsigned char label[0x40] = {0};
    _Alignas(16) unsigned char item[0x100] = {0};
    NATIVE(TextLookup, TEXT_LOOKUP)(label, KEYGUIDE_FMG, TEXT_DUPLICATE);
    /* The texts come with the respec mod (tools/respec_mod.py); without them the lookup returns a
     * placeholder (raw text pointer +0 NULL): no button then. */
    uint64_t text;
    memcpy(&text, label, 8);
    if (!text) {
        free_wstring(label, 0x10, 0x28, 0x30);
        if (!warned) printf("Saveslots: no \"Duplicate\" text (the respec mod is missing): no Duplicate button\n");
        warned = 1;
        return;
    }
    NATIVE(KeyguideCtor, KEYGUIDE_TRIANGLE)(item, label);
    free_wstring(label, 0x10, 0x28, 0x30);
    struct {
        void *vtable;
        uint64_t dialog;
        uint64_t unused[2];
        void *target;
    } action = {&action_vtable[1], dialog, {0, 0}, NULL};
    action.target = &action;
    NATIVE(BindingAdd, BINDING_ADD)(dialog, item, &action, (void *)(uintptr_t)regs->rbx);
    /* The item's two strings and its functor, as the game frees the Delete item (0x1b42fd2..). */
    free_wstring(item, 0x88, 0xa0, 0xa8);
    free_wstring(item, 0x48, 0x60, 0x68);
    uint64_t target;
    memcpy(&target, item + 0x20, 8);
    if (target) {
        Destroy destroy = (Destroy)(uintptr_t)((uint64_t *)(uintptr_t)(*(uint64_t *)(uintptr_t)target))[4];
        destroy((void *)(uintptr_t)target, target != (uint64_t)(uintptr_t)item);
    }
}

/* In the Delete action, as job A captures the slot: tagged when it is our Duplicate. */
static void delete_slot(HookRegs *regs) {
    if (!duplicating) return;
    regs->r14 = (regs->r14 & 0xFF) | (free_slot(summaries()) >= 0 ? TAG_DUPLICATE : TAG_NO_SLOT);
}

/* Job A, after Yes: a tagged slot is ours. The game's code after this does nothing with it (> 9). */
static void clear_slot(HookRegs *regs) {
    uint32_t slot = (uint32_t)regs->rax;
    if (slot & TAG_DUPLICATE) duplicate((int)(slot & 0xFF));
}

int saveslots_text(uint32_t fmg, uint32_t *id) {
    if (!installed || !duplicating || fmg != DIALOG_FMG || *id != DELETE_CONFIRM) return 0;
    *id = free_slot(summaries()) >= 0 ? TEXT_CONFIRM : TEXT_NO_SLOT;
    return 1;
}

typedef struct {
    const char *name;
    uint64_t offset;
    unsigned length;
    unsigned char bytes[16];
    HookHandler handler;
} Site;

static const Site sites[] = {
    {"Load Game: after the Delete binding", KEYGUIDE_ADDED, 7, {0x48,0x8B,0xBD,0x60,0xE9,0xFF,0xFF}, keyguide_added},
    {"Delete action: job A's slot", DELETE_SLOT, 7, {0x44,0x89,0xB5,0x88,0xFE,0xFF,0xFF}, delete_slot},
    {"Delete job A: exists[slot] = 0", CLEAR_SLOT, 7, {0x48,0x8B,0x51,0x58,0x48,0x85,0xD2}, clear_slot},
    /* Called, checked only. */
    {"text lookup", TEXT_LOOKUP, 4, {0x55,0x48,0x89,0xE5}, NULL},
    {"Triangle key guide item", KEYGUIDE_TRIANGLE, 4, {0x55,0x48,0x89,0xE5}, NULL},
    {"binding add", BINDING_ADD, 4, {0x55,0x48,0x89,0xE5}, NULL},
    {"Delete action", DELETE_ACTION, 4, {0x55,0x48,0x89,0xE5}, NULL},
    {"Delete job A", 0x01B45DE0, 7, {0x8B,0x47,0x08,0x48,0x8D,0x0D,0x46}, NULL},
};

const char *saveslots_install(unsigned char *image, uint64_t size, int text_hook) {
    const char *env = getenv("BB_SAVESLOTS");
    if (env && !strcmp(env, "0")) return NULL;
    if (!text_hook) {
        /* Without the text hook the box would say "delete": no Duplicate then. */
        printf("Saveslots: off: the menu text hook (runtime_respec.c, BB_RESPEC) is not installed\n");
        return NULL;
    }
    size_t count = sizeof(sites) / sizeof(sites[0]), hooks = 0;
    for (size_t i = 0; i < count; ++i) {
        const Site *s = &sites[i];
        if (s->offset > size || s->length > size - s->offset || memcmp(image + s->offset, s->bytes, s->length)) {
            printf("Saveslots: off: site \"%s\" at %#llx does not match Bloodborne 1.09\n", s->name,
                   (unsigned long long)s->offset);
            return NULL;
        }
    }
    image_base = (uint64_t)(uintptr_t)image;
    action_vtable[0] = NULL; /* the game's has no type info either */
    action_vtable[1] = (void *)(uintptr_t)action_clone;
    action_vtable[2] = (void *)(uintptr_t)action_clone;
    action_vtable[3] = (void *)(uintptr_t)action_call;
    action_vtable[4] = (void *)(uintptr_t)(image_base + 0x01B439A0);
    action_vtable[5] = (void *)(uintptr_t)(image_base + FN_DESTROY);
    action_vtable[6] = (void *)(uintptr_t)(image_base + 0x01B439E0);
    action_vtable[7] = (void *)(uintptr_t)(image_base + 0x01B439F0);
    action_vtable[8] = (void *)(uintptr_t)(image_base + 0x01B43A00);
    const char *failure = hooks_init(image, size);
    for (size_t i = 0; !failure && i < count; ++i)
        if (sites[i].handler) {
            failure = hooks_install(sites[i].offset, sites[i].bytes, sites[i].length, sites[i].handler);
            ++hooks;
        }
    if (!failure) failure = hooks_seal();
    if (failure) return failure;
    installed = 1;
    printf("Saveslots: %zu hooks installed (Load Game: Triangle duplicates a profile)\n", hooks);
    return NULL;
}
