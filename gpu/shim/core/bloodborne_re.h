// bbcoop: the seamless co-op calls shadp2p's Http and NpMatching2 make into its bloodborne_re.cpp.
// Placement state lives in bbcoop_placement.cpp; the game-side hooks (C runtime) reach it through
// the extern "C" functions declared there.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Core::Bloodborne {
std::optional<std::string> GetSeamlessHostPlacementHeader();
bool SetSeamlessHostPlacementHeader(std::string_view value);
void ClearSeamlessHostPlacementHeader();
void TraceMatching2LeaveRoom(std::uintptr_t return_address, std::uint64_t room_id);
} // namespace Core::Bloodborne
