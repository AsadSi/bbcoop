/* bbcoop: the host's live placement (sent with summon requests) and the placement a guest
 * received (applied when it is inserted), shared between the HTTP layer (C++) and the game
 * hooks (C runtime). Wire format from shadp2p's X-ShadPS4-Bloodborne-Host-Placement header. */
#ifndef BBCOOP_PLACEMENT_H
#define BBCOOP_PLACEMENT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint32_t packed_region;
    float x, y, z, heading;
    int32_t area;
} BbcoopPlacement;

/* Host hook: the placement to advertise; overwrites the previous one. */
void bbcoop_placement_set_host(const BbcoopPlacement *placement);
/* Guest hook: copies the received placement and marks it consumed. Returns 0 when there is
 * none, or it was already consumed. */
int bbcoop_placement_take_received(BbcoopPlacement *out);
#ifdef __cplusplus
}
#endif
#endif
