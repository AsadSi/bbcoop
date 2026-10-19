// SPDX-License-Identifier: GPL-2.0-or-later
// bbcoop: mirrored loot. Every pickup, enemy drop and boss drop a hunter gets is given to every
// other hunter in the session (docs/bbcoop/brief.md, Loot). Code: shadps4/core/bbcoop_loot.inc, included
// by bloodborne_re.cpp. Reverse engineering: docs/bbcoop/coop_loot_re.md.
#pragma once

#include <cstdint>

namespace Core::Bloodborne {

// Relay packet "SHAD" 0x28 (BBSC uses 0x20 and 0x22..0x27).
inline constexpr std::uint8_t kBbcoopLootPacketType = 0x28;

// True once mirroring runs: SHADPS4_BBSC_MIRROR_LOOT (on by default with BBSC, bbsc_persistence.h)
// and the ItemGet hook went in.
bool BbcoopLootMirroring();
// Signaling receive thread: one "items obtained" packet from another hunter. Only stored; the game
// tick gives the items.
void OnBbcoopLootPacket(const void* data, std::uint32_t size);
// Game thread: from BBSC's install and game tick.
void InstallBbcoopLoot();
void TickBbcoopLoot();

} // namespace Core::Bloodborne
