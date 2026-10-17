/* Explicit guest timeval layout; host pointers never escape this boundary. */
#define _DEFAULT_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <pthread.h> /* bbcoop */
#include <time.h> /* bbcoop */
typedef struct { int64_t seconds, microseconds; } GuestTimeval;
typedef struct { int32_t minuteswest, dsttime; } GuestTimezone;
/* bbcoop: the game times its frames with gettimeofday, so a wall-clock step (NTP, resume) became one
 * unclamped frame time. It reads the wall clock at the first call plus monotonic time since;
 * BB_WALL_CLOCK=1 reads the wall clock every time. */
static pthread_once_t clock_once = PTHREAD_ONCE_INIT;
static int64_t clock_offset_us = -1; /* wall minus monotonic, in microseconds; -1: wall clock */
static void clock_init(void) {
    const char *env = getenv("BB_WALL_CLOCK");
    struct timeval wall;
    struct timespec mono;
    if ((env && !strcmp(env, "1")) || gettimeofday(&wall, NULL) || clock_gettime(CLOCK_MONOTONIC, &mono)) return;
    clock_offset_us = (int64_t)wall.tv_sec * 1000000 + wall.tv_usec - ((int64_t)mono.tv_sec * 1000000 + mono.tv_nsec / 1000);
}
static ABI int guest_gettimeofday(GuestTimeval *value,GuestTimezone *zone) {
    struct timeval t;
    struct timezone z;
    if (gettimeofday(&t,&z)) {
        /* Guest errno/TLS failure path is not implemented yet. */
        perror("STOP: host gettimeofday failed"); exit(21);
    }
    pthread_once(&clock_once, clock_init); /* bbcoop */
    struct timespec mono;
    if (clock_offset_us >= 0 && !clock_gettime(CLOCK_MONOTONIC, &mono)) {
        int64_t us = (int64_t)mono.tv_sec * 1000000 + mono.tv_nsec / 1000 + clock_offset_us;
        t.tv_sec = us / 1000000; t.tv_usec = us % 1000000;
    }
    if (value) { value->seconds=t.tv_sec; value->microseconds=t.tv_usec; }
    if (zone) { zone->minuteswest=z.tz_minuteswest; zone->dsttime=z.tz_dsttime; }
    return 0;
}
uintptr_t runtime_time_resolve(const char *name) {
    if (runtime_nid_is(name, "n88vx3C5nW8")) return (uintptr_t)guest_gettimeofday;
    return 0;
}
