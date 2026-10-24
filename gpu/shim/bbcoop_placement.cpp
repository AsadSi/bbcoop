// bbcoop: placement header state (see bbcoop_placement.h). Encoding and validation follow
// shadp2p's Get/SetSeamlessHostPlacementHeader (src/core/bloodborne_re.cpp), so a shadNet
// server relays it unchanged.
#include <bit>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <array>
#include "bbcoop_placement.h"
#include "common/logging/log.h"
#include "core/bloodborne_re.h"

namespace {
std::mutex mutex;
std::optional<BbcoopPlacement> host;
std::optional<BbcoopPlacement> received;
bool received_consumed{};

bool Same(const BbcoopPlacement& a, const BbcoopPlacement& b) {
    return a.packed_region == b.packed_region && std::bit_cast<uint32_t>(a.x) == std::bit_cast<uint32_t>(b.x) &&
           std::bit_cast<uint32_t>(a.y) == std::bit_cast<uint32_t>(b.y) &&
           std::bit_cast<uint32_t>(a.z) == std::bit_cast<uint32_t>(b.z) &&
           std::bit_cast<uint32_t>(a.heading) == std::bit_cast<uint32_t>(b.heading) && a.area == b.area;
}
} // namespace

extern "C" void bbcoop_placement_set_host(const BbcoopPlacement* placement) {
    std::scoped_lock lock{mutex};
    if (placement) host = *placement;
    else host.reset();
}

extern "C" int bbcoop_placement_take_received(BbcoopPlacement* out) {
    std::scoped_lock lock{mutex};
    if (!received || received_consumed) return 0;
    *out = *received;
    received_consumed = true;
    return 1;
}

extern "C" int bbcoop_placement_received(BbcoopPlacement* out, int* consumed) {
    std::scoped_lock lock{mutex};
    if (!received) return 0;
    *out = *received;
    *consumed = received_consumed;
    return 1;
}

extern "C" void bbcoop_placement_consume(const BbcoopPlacement* placement) {
    std::scoped_lock lock{mutex};
    if (received && Same(*received, *placement)) received_consumed = true;
}

// bbcoop: on Windows the online module is linked into the same executable and its own (BBSC's) are the
// only ones called, as on Linux, where each module binds its own (-Bsymbolic).
#ifndef _WIN32
namespace Core::Bloodborne {

std::optional<std::string> GetSeamlessHostPlacementHeader() {
    std::optional<BbcoopPlacement> placement;
    {
        std::scoped_lock lock{mutex};
        placement = host;
    }
    if (!placement) return std::nullopt;
    std::ostringstream out;
    out << "1," << std::hex << std::setfill('0') << std::setw(8) << placement->packed_region << ','
        << std::setw(8) << std::bit_cast<uint32_t>(placement->x) << ',' << std::setw(8)
        << std::bit_cast<uint32_t>(placement->y) << ',' << std::setw(8) << std::bit_cast<uint32_t>(placement->z)
        << ',' << std::setw(8) << std::bit_cast<uint32_t>(placement->heading) << ',' << std::dec
        << placement->area;
    return out.str();
}

bool SetSeamlessHostPlacementHeader(std::string_view value) {
    std::array<std::string_view, 7> fields{};
    size_t begin = 0;
    for (size_t i = 0; i < fields.size(); ++i) {
        const size_t end = value.find(',', begin);
        const bool last = i + 1 == fields.size();
        if ((last && end != std::string_view::npos) || (!last && end == std::string_view::npos)) return false;
        fields[i] = value.substr(begin, end == std::string_view::npos ? value.size() - begin : end - begin);
        begin = end == std::string_view::npos ? value.size() : end + 1;
    }
    if (fields[0] != "1") return false;
    auto parse_u32 = [](std::string_view field, uint32_t& output) {
        const auto [end, error] = std::from_chars(field.data(), field.data() + field.size(), output, 16);
        return error == std::errc{} && end == field.data() + field.size();
    };
    BbcoopPlacement placement{};
    std::array<uint32_t, 4> bits{};
    if (!parse_u32(fields[1], placement.packed_region) || !parse_u32(fields[2], bits[0]) ||
        !parse_u32(fields[3], bits[1]) || !parse_u32(fields[4], bits[2]) || !parse_u32(fields[5], bits[3]))
        return false;
    const auto [area_end, area_error] =
        std::from_chars(fields[6].data(), fields[6].data() + fields[6].size(), placement.area, 10);
    if (area_error != std::errc{} || area_end != fields[6].data() + fields[6].size()) return false;
    placement.x = std::bit_cast<float>(bits[0]);
    placement.y = std::bit_cast<float>(bits[1]);
    placement.z = std::bit_cast<float>(bits[2]);
    placement.heading = std::bit_cast<float>(bits[3]);
    if (placement.packed_region == 0 || placement.packed_region == UINT32_MAX || !std::isfinite(placement.x) ||
        !std::isfinite(placement.y) || !std::isfinite(placement.z) || !std::isfinite(placement.heading))
        return false;
    std::scoped_lock lock{mutex};
    if (!received || !Same(*received, placement)) received_consumed = false;
    received = placement;
    return true;
}

void ClearSeamlessHostPlacementHeader() {
    std::scoped_lock lock{mutex};
    received.reset();
    received_consumed = false;
}

void TraceMatching2LeaveRoom(std::uintptr_t return_address, std::uint64_t room_id) {
    LOG_INFO(Lib_NpMatching2, "Matching2 leave room={} caller={:#x}", room_id, return_address);
}

} // namespace Core::Bloodborne
#endif
