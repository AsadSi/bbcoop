/* bbcoop: the co-op sync channel. Messages the game's own netcode does not carry (lamp travel,
 * progress flags, loot copies, deaths) go to every other player in the co-op session through
 * the host's server (server/src/cmd_coop.cpp), in order. Payloads are opaque here; their format
 * belongs to the C runtime's co-op code. */
#ifndef BBCOOP_SYNC_H
#define BBCOOP_SYNC_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { BBCOOP_SYNC_MAX = 4096, BBCOOP_SYNC_NAME = 17 };

/* Sends to everyone else in the session. 0 when queued, -1 when offline or too large. */
int bbcoop_sync_send(const void *data, uint32_t size);
/* Takes the oldest received message: copies the sender's name (NUL-terminated) and up to
 * `capacity` bytes, returns the message size, or -1 when none is waiting. Any thread. */
int bbcoop_sync_receive(char from[BBCOOP_SYNC_NAME], void *data, uint32_t capacity);
/* The shadNet client's reader thread hands messages in here. */
void bbcoop_sync_deliver(const char *from, const void *data, uint32_t size);
#ifdef __cplusplus
}
#endif
#endif
