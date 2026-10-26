/* bbcoop: a hang report on Windows, where probe.c's watchdog (signals, /proc) does not exist. When the
 * game has presented frames and then presents none for BB_HANG_REPORT seconds (default 15), every
 * thread of the process is paused in turn and written to the log once: its name, where it is
 * (game+offset inside the game's image, else module+offset), the argument registers and the rbp
 * frame chain. Which wait a stuck game is in is then read from the log of one run (2026-10-26: a
 * black screen at character creation on an AMD PC). BB_HANG_REPORT=0: off. Again after frames
 * resume and stop once more. */
#include <stdint.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t bbgpu_flips(void);

static uintptr_t image_base;
static uint64_t image_size;
static DWORD delay_ms = 15000;

/* A thread's state, copied while it is paused: nothing that takes a lock runs meanwhile. */
typedef struct {
    CONTEXT context;
    uintptr_t frames[16];
    int frame_count;
} Snapshot;

static int read_memory(uintptr_t address, void *out, size_t size) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (const void *)address, out, size, &got) && got == size;
}

static void where(uintptr_t rip, char *out, size_t size) {
    if (rip - image_base < image_size) {
        snprintf(out, size, "game+%#llx", (unsigned long long)(rip - image_base));
        return;
    }
    HMODULE module = NULL;
    char path[MAX_PATH];
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)rip, &module) &&
        GetModuleFileNameA(module, path, sizeof(path))) {
        const char *name = strrchr(path, '\\');
        snprintf(out, size, "%s+%#llx", name ? name + 1 : path, (unsigned long long)(rip - (uintptr_t)module));
        return;
    }
    snprintf(out, size, "%#llx", (unsigned long long)rip);
}

static void snapshot(HANDLE thread, Snapshot *s) {
    s->frame_count = -1;
    if (SuspendThread(thread) == (DWORD)-1) return;
    s->context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (GetThreadContext(thread, &s->context)) {
        s->frame_count = 0;
        uintptr_t rbp = (uintptr_t)s->context.Rbp;
        for (int i = 0; i < 16 && rbp; ++i) {
            uintptr_t frame[2];
            if (!read_memory(rbp, frame, sizeof(frame)) || frame[0] <= rbp) break;
            s->frames[s->frame_count++] = frame[1];
            rbp = frame[0];
        }
    }
    ResumeThread(thread);
}

typedef HRESULT(WINAPI *GetDescription)(HANDLE, PWSTR *);

static void report(uint64_t flips, DWORD stalled_ms) {
    printf("Hang report: no new frame for %lu s (after %llu frames); every thread:\n",
           (unsigned long)(stalled_ms / 1000), (unsigned long long)flips);
    const GetDescription get_description =
        (GetDescription)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
    HANDLE threads = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (threads == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 entry = {.dwSize = sizeof(entry)};
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    for (BOOL more = Thread32First(threads, &entry); more; more = Thread32Next(threads, &entry)) {
        if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) continue;
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION,
                                   FALSE, entry.th32ThreadID);
        if (!thread) continue;
        char name[64] = "?";
        PWSTR wide = NULL;
        if (get_description && SUCCEEDED(get_description(thread, &wide)) && wide) {
            if (*wide) WideCharToMultiByte(CP_UTF8, 0, wide, -1, name, sizeof(name), NULL, NULL);
            LocalFree(wide);
        }
        Snapshot s;
        snapshot(thread, &s);
        CloseHandle(thread);
        if (s.frame_count < 0) continue;
        char rip[160];
        where((uintptr_t)s.context.Rip, rip, sizeof(rip));
        printf("  thread %lu %s: %s rdi=%#llx rsi=%#llx rdx=%#llx rcx=%#llx rbx=%#llx\n",
               (unsigned long)entry.th32ThreadID, name, rip, (unsigned long long)s.context.Rdi,
               (unsigned long long)s.context.Rsi, (unsigned long long)s.context.Rdx,
               (unsigned long long)s.context.Rcx, (unsigned long long)s.context.Rbx);
        for (int i = 0; i < s.frame_count; ++i) {
            where(s.frames[i], rip, sizeof(rip));
            printf("    from %s\n", rip);
        }
    }
    CloseHandle(threads);
    fflush(stdout);
}

static DWORD WINAPI watch(void *unused) {
    (void)unused;
    uint64_t seen = 0;
    DWORD since = GetTickCount();
    int reported = 0;
    for (;;) {
        Sleep(1000);
        const uint64_t flips = bbgpu_flips();
        const DWORD now = GetTickCount();
        if (flips != seen) {
            seen = flips;
            since = now;
            reported = 0;
        } else if (flips && !reported && now - since >= delay_ms) {
            report(flips, now - since);
            reported = 1;
        }
    }
    return 0;
}

void runtime_hang_report_start(const unsigned char *image, uint64_t size) {
    const char *env = getenv("BB_HANG_REPORT");
    if (env && *env) {
        const long seconds = strtol(env, NULL, 10);
        if (seconds <= 0) return;
        delay_ms = (DWORD)seconds * 1000;
    }
    image_base = (uintptr_t)image;
    image_size = size;
    HANDLE thread = CreateThread(NULL, 0, watch, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
}
#else
void runtime_hang_report_start(const unsigned char *image, uint64_t size) { (void)image; (void)size; }
#endif
