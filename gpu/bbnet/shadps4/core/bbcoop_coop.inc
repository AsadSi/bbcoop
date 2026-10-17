// SPDX-License-Identifier: GPL-2.0-or-later
// bbcoop: co-op rules on top of BBSC. Included into bloodborne_re.cpp's anonymous namespace (right
// before BbscRespawnInPlace), so it uses BBSC's own helpers and state; the BBSC side has only small
// `// bbcoop:` calls into it. RE notes: docs/coop_respawn_re.md.
//
// ---- A guest's death: respawn at the host's last lamp, in any map --------------------------------
// Asad's rule (docs/brief.md, "Guest death"): a dead guest respawns at the HOST's last lamp and stays
// in the session. BBSC already does that without a load when the host's lamp is in a map loaded on
// the guest's side (BbscRespawnInPlace, the host sends its lamp with every follow packet); when it is
// not, BBSC puts the guest beside the host or at the nearest lamp of its own map. This adds the
// missing case: the guest LOADS into the map of the host's lamp and the game places it there, then
// BBSC's revive runs on the new character (it now finds the lamp loaded). Only for a death outside a
// boss fight (TickBbscSpectate's "no boss fight"): a boss fight is waited out as BBSC does, and the
// respawn after the kill stays beside the host / at a loaded lamp, next to the arena, where the
// party is. Anything unknown or failing falls back to BBSC's own respawn.
//
// The load (CONFIRMED statically, docs/coop_respawn_re.md):
//   * the stage flow's placement choice 0x01943F10 takes, before the lamp (+0x1524) and the forced
//     warp (+0x1520), the respawn-point warp of event 2003[49] (WarpToRespawnPoint 0x013CDF30):
//     global+0x1538 = mode (1 lamp travel: also makes it the last lamp; 2 plain), +0x153C = respawn
//     point id (the id global+0x1528 holds), +0x1540 = the warp row's second value. Only +0x1523
//     (place in the Dream) and +0x1521 (summon placement) come first, so both are cleared.
//   * the map: +0xC, from the point id the way the lamp respawn does (0x01944E3C): area =
//     id / 100000 % 100, block = id / 10000 % 10 (area 29: Chalice Dungeons, never used here).
//   * a guest in a session: summon reload phase [[0x0553D6D0]+0x84] = 3 keeps the session through
//     the load and makes the load step take its map from global+0x14C4 (BBSC's RequestBbscSessionWarp
//     does the same for its follow loads).
//   * StageTransition 0x013CDE30 starts it; 0x0156D300 clears +0x1538/+0x1520.. after the placement.
// Switch: SHADPS4_BBSC_DEATH_TO_HOST_LAMP (on by default with BBSC, =0 off: BBSC's own respawn).
// Test seam: BB_TEST_GUEST_DEATH=<respawn point id> (throwaway profiles only): once in game, the local
// player is killed once after 10 s, and that death - solo, in its own world - is handled as a guest
// death whose host's last lamp is that point. See docs/coop_respawn_re.md "Testing".

constexpr std::string_view BbcoopHostLampWhy = "loaded at the host's lamp";
constexpr std::string_view BbcoopHostLampCover = "host lamp"; // the load cover's name
constexpr s64 BbcoopLampLoadStartMs = 10'000;  // the load must have begun by then
constexpr s64 BbcoopLampLoadLimitMs = 120'000; // whatever happens, BBSC's rules take over again
constexpr s64 BbcoopLampSettleMs = 1'000;      // after the load, before the revive

struct BbcoopLampLoad {
    bool pending = false;
    bool load_seen = false;
    u64 old_chr = 0;
    s32 point = -1;
    u32 map = 0;
    s64 since_ms = 0;
    s64 settled_since_ms = 0;
};
BbcoopLampLoad bbcoop_lamp_load;

bool BbcoopDeathToHostLampEnabled() {
    static const bool enabled = EnvFlagEnabled("SHADPS4_BBSC_DEATH_TO_HOST_LAMP");
    return enabled;
}

// BB_TEST_GUEST_DEATH: the host's lamp the test pretends to have (-1: no test).
s32 BbcoopTestGuestLamp() {
    static const s32 point = [] {
        const char* value = std::getenv("BB_TEST_GUEST_DEATH");
        const s32 parsed = value != nullptr ? static_cast<s32>(std::strtol(value, nullptr, 10)) : 0;
        if (parsed <= 0 || parsed % 10000 < 1000) {
            return -1; // a respawn point id is <area><block><4 digits>
        }
        LOG_INFO(Debug, "bbcoop test: BB_TEST_GUEST_DEATH - a local death is a guest death with "
                        "the host's last lamp at point {}",
                 parsed);
        return parsed;
    }();
    return point;
}

bool bbcoop_test_guest_death = false; // the seam's death is being handled as a guest's

bool BbcoopTestGuestDeathActive() {
    return bbcoop_test_guest_death;
}

// The map a respawn point lies in, as the lamp respawn computes it (0x01944E3C); 0 for none.
u32 BbcoopRespawnPointMap(s32 point) {
    if (point <= 0) {
        return 0;
    }
    const u32 area = static_cast<u32>(point / 100000 % 100);
    const u32 block = static_cast<u32>(point / 10000 % 10);
    if (area == 0 || area == 29) {
        return 0;
    }
    return area << 24 | block << 16;
}

// The guest's respawn after a death outside a boss fight: when the host's last lamp is in a map not
// loaded here, load there. True when the load was requested (the death is then ours until the new
// character stands at the lamp, TickBbcoopHostLampRespawn); false leaves it to BBSC.
bool BbcoopRespawnAtHostLamp() {
    if (!BbcoopDeathToHostLampEnabled() || image_base == 0) {
        return false;
    }
    const u64 global = GetGlobalState();
    const bool test = bbcoop_test_guest_death;
    const bool guest = GetSessionState() == 6 && ReadablePointer(global, 0x1598) &&
                       ReadValue<u8>(global, 0x1592) == 0 && GetLocalPlayerType() != 2;
    if ((!guest && !test) || !ReadablePointer(global, 0x1598) || ReadValue<u8>(global, 0x08) != 0 ||
        !IsSprjLuaEventManReady()) {
        return false;
    }
    s32 point = -1;
    {
        std::scoped_lock lock{bbsc_follow_mutex};
        if (bbsc_follow_latest.received_ms != 0) {
            point = bbsc_follow_latest.lamp_point;
        }
    }
    if (point == -1) {
        LOG_INFO(Debug, "bbcoop death: the host's last lamp is unknown - BBSC's respawn");
        return false;
    }
    if (ResolveLampPoint(point).has_value()) {
        return false; // in a loaded map: BBSC's respawn without a load goes there
    }
    const u32 map = BbcoopRespawnPointMap(point);
    if (!IsUsablePackedMap(map)) {
        LOG_INFO(Debug, "bbcoop death: the host's last lamp {} names no map - BBSC's respawn", point);
        return false;
    }
    // The placement choice (0x01943F10) must reach the respawn-point warp: no Dream placement
    // (+0x1523), no summon placement (+0x1521), and no forced warp home left over (+0x1520).
    WriteValue<u8>(global, 0x1520, 0);
    WriteValue<u8>(global, 0x1521, 0);
    WriteValue<u8>(global, 0x1523, 0);
    // The warp row's second value (+0x1540, the arrival setting; the last lamp is the pair
    // +0x1528/+0x152C) is not sent by the host: the guest's own stands in (HYPOTHESIS that any
    // value places the player; 0 did in the test, docs/coop_respawn_re.md).
    const s32 own_lamp = ReadValue<s32>(global, 0x1528);
    const s32 second = own_lamp != -1 ? ReadValue<s32>(global, 0x152C) : -1;
    WriteValue<u32>(global, 0x0C, map);
    WriteValue<s32>(global, 0x1538, 2); // plain warp: the guest's own last lamp stays its own
    WriteValue<s32>(global, 0x153C, point);
    WriteValue<s32>(global, 0x1540, second);
    if (guest) {
        // As RequestBbscSessionWarp: the session survives the load, which goes to +0x14C4.
        WriteValue<u32>(global, 0x14C4, map);
        const u64 rules = ReadValue<u64>(image_base + SummonSessionRulesPointerOffset, 0);
        if (ReadablePointer(rules, 0x88) && HasMemoryAccess(rules + 0x84, 4, MemoryProt::CpuWrite)) {
            WriteValue<s32>(rules, 0x84, 3);
        }
        if (BbscHostWorldFlagsReady(map)) {
            ApplyBbscHostWorldFlags(map, "before the load to the host's lamp");
        }
        ApplyBbscGuestLootView(map, "before the load to the host's lamp");
        StartBbscLoadCover(BbcoopHostLampCover);
    }
    using StageTransition = u64 PS4_SYSV_ABI (*)();
    const u64 result = reinterpret_cast<StageTransition>(image_base + StageTransitionOffset)();
    bbcoop_lamp_load = {};
    bbcoop_lamp_load.pending = true;
    bbcoop_lamp_load.old_chr = GetLocalPlayerCharacter();
    bbcoop_lamp_load.point = point;
    bbcoop_lamp_load.map = map;
    bbcoop_lamp_load.since_ms = SteadyNowMs();
    // BBSC's death stays held (spectate, so neither its watchdog nor its revive acts) until the
    // new character stands at the lamp.
    bbsc_death_mode = BbscDeathMode::Spectate;
    bbsc_spectate_active = true;
    LOG_INFO(Debug,
             "bbcoop death: the host's last lamp {} is in map {:#x}, not loaded here (we are in "
             "{:#x}) - loading there{} (own last lamp {} / {}, result {})",
             point, map, GetCurrentPackedMap(), guest ? " in the session" : " (test)", own_lamp,
             second, result);
    return true;
}

// Runs first in TickBbscSpectate. True while the death is ours.
bool TickBbcoopHostLampRespawn() {
    if (!bbcoop_lamp_load.pending) {
        return false;
    }
    const s64 now = SteadyNowMs();
    const bool test = bbcoop_test_guest_death;
    const s32 session_state = GetSessionState();
    auto give_back = [&](std::string_view why) {
        LOG_WARNING(Debug, "bbcoop death: {} - BBSC's rules take over", why);
        bbcoop_lamp_load.pending = false;
        bbcoop_test_guest_death = false;
        if (test) {
            bbsc_spectate_active = false; // solo: BBSC asks for the game's own reload
            bbsc_request_death_reload = true;
        }
    };
    if (!test && session_state != 6) {
        give_back("the session ended during the load to the host's lamp");
        return false;
    }
    const u64 global = GetGlobalState();
    const u64 menu = ReadValue<u64>(image_base + MenuManPointerOffset, 0);
    const u64 chr = GetLocalPlayerCharacter();
    // StageTransition's request byte is consumed within moments, so the load is told by its
    // arrival in the lamp's map. (The loading screen state MenuMan+0x804 is no help: it stayed 3
    // after the load, with the level on screen.)
    const bool loading = ReadablePointer(global, 0x10) && ReadValue<u8>(global, 0x08) != 0;
    const u32 here = GetCurrentPackedMap();
    if (loading || here == bbcoop_lamp_load.map) {
        bbcoop_lamp_load.load_seen = true;
    }
    static s64 traced_ms = 0;
    if (now - traced_ms >= 5'000) {
        traced_ms = now;
        LOG_INFO(Debug, "bbcoop death: waiting for the load (map {:#x}, loading {}, request {}, "
                        "screen {}, character {:#x} was {:#x})",
                 here, loading, ReadablePointer(global, 0x10) ? ReadValue<u8>(global, 0x08) : 0xFF,
                 ReadablePointer(menu, 0x810) ? ReadValue<s32>(menu, 0x804) : -1, chr,
                 bbcoop_lamp_load.old_chr);
    }
    if (loading) {
        bbcoop_lamp_load.settled_since_ms = 0;
        return true;
    }
    if (now - bbcoop_lamp_load.since_ms > BbcoopLampLoadLimitMs) {
        give_back("no character at the host's lamp after 2 minutes");
        return false;
    }
    if (!bbcoop_lamp_load.load_seen) {
        if (now - bbcoop_lamp_load.since_ms < BbcoopLampLoadStartMs) {
            return true;
        }
        // Nothing loaded: revive here the way BBSC would have (the lamp is still not loaded, so
        // that is beside the host or the nearest lamp).
        LOG_WARNING(Debug, "bbcoop death: the load to the host's lamp did not start - BBSC's respawn");
        bbcoop_lamp_load.pending = false;
        if (StartBbscDeathCurtain("host's lamp load did not start") ||
            BbscRespawnInPlace("host's lamp load did not start")) {
            bbsc_spectate_active = false;
            return true;
        }
        give_back("respawn in place failed");
        return false;
    }
    if (chr == 0 || here != bbcoop_lamp_load.map || !IsSprjLuaEventManReady() ||
        !ReadCharacterTransform(chr).has_value()) {
        bbcoop_lamp_load.settled_since_ms = 0;
        return true; // the new character is not there yet
    }
    if (bbcoop_lamp_load.settled_since_ms == 0) {
        bbcoop_lamp_load.settled_since_ms = now;
        const auto at = ReadCharacterTransform(chr);
        const auto lamp = ResolveLampPoint(bbcoop_lamp_load.point);
        LOG_INFO(Debug,
                 "bbcoop death: loaded into map {:#x} (wanted {:#x}), the game placed us at "
                 "({:.2f}, {:.2f}, {:.2f}), the lamp {} is at ({:.2f}, {:.2f}, {:.2f}); hp {}, own "
                 "last lamp now {}",
                 GetCurrentPackedMap(), bbcoop_lamp_load.map, at->position[0], at->position[1],
                 at->position[2], bbcoop_lamp_load.point, lamp ? lamp->position[0] : 0.0f,
                 lamp ? lamp->position[1] : 0.0f, lamp ? lamp->position[2] : 0.0f,
                 GetCharacterHp(chr), ReadValue<s32>(global, 0x1528));
        return true;
    }
    if (now - bbcoop_lamp_load.settled_since_ms < BbcoopLampSettleMs) {
        return true;
    }
    // BBSC's revive on the new character: the host's lamp is loaded now, so it stands there with
    // full HP and the death's leftovers cleared ("You respawned at the host's lamp.").
    bbcoop_lamp_load.pending = false;
    const bool revived = BbscRespawnInPlace(BbcoopHostLampWhy);
    bbcoop_test_guest_death = false;
    bbsc_spectate_active = false;
    if (!revived) {
        LOG_WARNING(Debug, "bbcoop death: revive at the host's lamp failed - BBSC's watchdog revives");
    }
    return true;
}

// The test seam's death (BB_TEST_GUEST_DEATH), from BbscSpectateDeathHook's solo branch: held like a
// guest's kept death, with the test lamp as the host's (a follow message as the host would send it).
bool BbcoopTestGuestDeath(std::string_view name) {
    const s32 point = BbcoopTestGuestLamp();
    if (point == -1 || bbcoop_test_guest_death) {
        return false;
    }
    {
        std::scoped_lock lock{bbsc_follow_mutex};
        bbsc_follow_latest = {};
        bbsc_follow_latest.packed_map = GetCurrentPackedMap();
        bbsc_follow_latest.lamp_point = point;
        bbsc_follow_latest.received_ms = SteadyNowMs();
    }
    bbcoop_test_guest_death = true;
    bbsc_spectate_name = name;
    bbsc_spectate_since_ms = SteadyNowMs();
    bbsc_spectate_chr = GetLocalPlayerCharacter();
    bbsc_death_mode = BbscDeathMode::RespawnNow;
    bbsc_spectate_active = true;
    LOG_INFO(Debug, "bbcoop test: {} handled as a guest death (host's last lamp {})", name, point);
    return true;
}

// The test seam's trigger, from TickBbscSpectate: the guest's "no boss fight" respawn, then (once
// per run) the kill that starts it all.
bool TickBbcoopTestGuestDeath() {
    static bool killed = false;
    static s64 alive_since_ms = 0;
    if (BbcoopTestGuestLamp() == -1) {
        return false;
    }
    if (bbcoop_test_guest_death && !bbcoop_lamp_load.pending && bbsc_spectate_active) {
        // As TickBbscSpectate's "no boss fight" branch (which needs a session and a partner).
        if (!BbcoopRespawnAtHostLamp() && !StartBbscDeathCurtain("test: no boss fight") &&
            !BbscRespawnInPlace("test: no boss fight")) {
            bbcoop_test_guest_death = false;
            return false;
        }
        if (!bbcoop_lamp_load.pending) {
            bbsc_spectate_active = false; // BBSC's own respawn took it (the lamp was loaded)
        }
        return true;
    }
    if (killed) {
        return false;
    }
    const u64 chr = GetLocalPlayerCharacter();
    const u64 global = GetGlobalState();
    const s64 now = SteadyNowMs();
    if (chr == 0 || GetCharacterHp(chr) <= 0 || !ReadablePointer(global, 0x10) ||
        ReadValue<u8>(global, 0x08) != 0 || !IsSprjLuaEventManReady()) {
        static s64 traced_ms = 0;
        if (now - traced_ms > 5'000) {
            traced_ms = now;
            LOG_INFO(Debug, "bbcoop test: not killing yet (chr {:#x} hp {} loading {} lua {})", chr,
                     chr != 0 ? GetCharacterHp(chr) : -1,
                     ReadablePointer(global, 0x10) ? ReadValue<u8>(global, 0x08) : 0xFF,
                     IsSprjLuaEventManReady());
        }
        alive_since_ms = 0;
        return false;
    }
    if (alive_since_ms == 0) {
        alive_since_ms = now;
    }
    if (now - alive_since_ms < 10'000) {
        return false;
    }
    killed = true;
    using SetHp = void PS4_SYSV_ABI (*)(u64 chr, s32 hp);
    reinterpret_cast<SetHp>(image_base + 0x018BCD40)(chr, 0);
    LOG_INFO(Debug, "bbcoop test: killed the local player in map {:#x} (own last lamp {} / {})",
             GetCurrentPackedMap(), ReadValue<s32>(global, 0x1528), ReadValue<s32>(global, 0x152C));
    return false;
}
