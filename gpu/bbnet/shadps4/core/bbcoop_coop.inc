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
    if (BbscMapArea(map) == BbscChaliceArea) {
        // Chalice co-op is parked: a load there has no dungeon layout (see TickBbscFollowHost).
        LOG_INFO(Debug, "bbcoop death: the host's last lamp {} is in a Chalice Dungeon - BBSC's respawn",
                 point);
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

// ---- ss.info format: the US release reads format 1 --------------------------------------------
// The game's ss.info parser 0x01EB65E0(?, std::string* text, u32* format, u32* lang, out), called
// from 0x01E89A21, looks for <msg%u>, then <lang%u> inside it, and so on, with %u the format the
// caller passes (its copy of [[0x0553B120]+0x9E8]). The US release (CUSA00900) passes 1, the EU
// one (CUSA03173) 2. Every community server (Seamless Hunt, BBSC's local one, NoDreamForHunter)
// serves format 2, the EU retail file, so the US game found no <msg1>, the parser failed and the
// title showed "Failed to resolve Bloodborne server status" (traced 2026-10-18: format 1, lang 2;
// with 2 it logs in and gets the server's notice). A byte-checked hook at the parser's entry turns
// 1 into 2. Switch: SHADPS4_BBSC_SS_FORMAT (on by default with BBSC, =0 off).
constexpr u64 SsInfoParserOffset = 0x01EB65E0;
constexpr std::array<u8, 6> SsInfoParserBytes{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57}; // push rbp; mov rbp,rsp; push r15

void PS4_SYSV_ABI BbcoopSsInfoFormatHook(u64 /*tag*/, const GuestRegisterSnapshot* registers) {
    // rdx: the caller's local (its stack is outside the memory map ReadValue checks).
    auto* format = registers ? reinterpret_cast<u32*>(registers->rdx) : nullptr;
    if (format != nullptr && *format == 1) {
        *format = 2;
        static bool told = false;
        if (!told) {
            told = true;
            LOG_INFO(Debug, "bbcoop: ss.info read as format 2 (the servers' EU file), not the US 1");
        }
    }
}

void InstallBbcoopSsInfoFormat() {
    if (image_base == 0 || !EnvFlagEnabled("SHADPS4_BBSC_SS_FORMAT")) {
        return;
    }
    if (!InstallGuestCodeHook(reinterpret_cast<void*>(image_base + SsInfoParserOffset),
                              std::span<const u8>{SsInfoParserBytes}, 0, BbcoopSsInfoFormatHook)) {
        LOG_ERROR(Debug, "bbcoop: ss.info parser at {:#x} differs - the US release stays offline",
                  SsInfoParserOffset);
    }
}

// ---- Alone, a hunter again (2026-10-25) ----------------------------------------------------------
// Report (Asad, 2026-10-25): after a co-op session he loaded his own world as a phantom - nothing in
// the Dream could be used (headstones, the Doll, doors), and BBSC's head count said "1 cooperator"
// while alone. The game left a guest's phantom type in his save: the window was closed during a
// session, while the v8 swap (ArmBbscReinsertV8) had the own-world restore type [[0x0553B188]+4] and
// [0x0553B130]+0x8C set to the phantom type. Solo, settled and alive for BbcoopSoloTypeSettleMs with
// a phantom type (1, 2, 12, 13: the types 0x015C9940 restores from) in any of those or in the slot
// (+0xA4) or character (+0x78) type, every one of them goes back to 0 (host). The game saves it
// with the next autosave. Only in our own world: after a session the guest stays in the host's
// world (still a phantom) until BBSC sends it home, and the repair fired there (2026-10-25 16:48:10,
// ten seconds before the trip home).
bool BbscOwnWorldNonGuest(u64 global);
constexpr s64 BbcoopSoloTypeSettleMs = 5'000;

bool BbcoopPhantomType(s32 type) {
    return type == 1 || type == 2 || type == 12 || type == 13;
}

void TickBbcoopSoloHunterType() {
    static s64 since_ms = 0;
    const s64 now = SteadyNowMs();
    const u64 global = GetGlobalState();
    const u64 slots = ReadValue<u64>(image_base + PlayerSlotManagerPointerOffset, 0);
    const u64 record = ReadValue<u64>(image_base + 0x0553B188, 0);
    const u64 world_chr = ReadValue<u64>(image_base + CandidateLocalStatePointerOffset, 0);
    const u64 player = ReadablePointer(world_chr, 0x68) ? ReadValue<u64>(world_chr, 0x60) : 0;
    const u64 local = ReadablePointer(slots, 0x90) ? ReadValue<u64>(slots, 0x08) : 0;
    if (image_base == 0 || bbsc_v8_pending || GetSessionState() != 0 ||
        !ReadablePointer(global, 0x10) || ReadValue<u8>(global, 0x08) != 0 ||
        !BbscOwnWorldNonGuest(global) ||
        GetBbscLocalPlayerHp() <= 0 || !ReadablePointer(record, 0x08) ||
        !ReadablePointer(local, 0xA8) || !ReadablePointer(player, 0x80)) {
        since_ms = 0;
        return;
    }
    const s32 restore = ReadValue<s32>(record, 4);
    const s32 queued = ReadValue<s32>(slots, 0x8C);
    const s32 slot = ReadValue<s32>(local, 0xA4);
    const s32 chr = ReadValue<s32>(player, 0x78);
    if (!BbcoopPhantomType(restore) && !BbcoopPhantomType(queued) && !BbcoopPhantomType(slot) &&
        !BbcoopPhantomType(chr)) {
        since_ms = 0;
        return;
    }
    if (since_ms == 0) {
        since_ms = now;
    }
    if (now - since_ms < BbcoopSoloTypeSettleMs) {
        return;
    }
    since_ms = 0;
    const auto reset = [](u64 base, u64 offset, s32 type) {
        if (BbcoopPhantomType(type) && HasMemoryAccess(base + offset, 4, MemoryProt::CpuWrite)) {
            WriteValue<s32>(base, offset, 0);
        }
    };
    reset(record, 4, restore);
    reset(slots, 0x8C, queued);
    reset(local, 0xA4, slot);
    reset(player, 0x78, chr);
    LOG_WARNING(Debug, "bbcoop: alone with a phantom type (restore {}, queued {}, slot {}, character {}) "
                       "- back to a hunter (0)", restore, queued, slot, chr);
    BBSC_TIMELINE("REPAIR", "alone with a phantom type (restore {}, queued {}, slot {}, character {}) - "
                            "back to a hunter (0)", restore, queued, slot, chr);
}

// Same report: still nothing usable in the Dream once the type was back to 0. The leave scripts set
// event flag 4047 ("leaving") and the Lua load wait (+0x8D) before the trip home, which clears them;
// the window closed mid-session, so 4047 stayed on in the save and every later load is locked
// (the soft lock of TickBbscStuckLeaveLocks, which only looks within 60 s after a session ends in the
// same run). Solo in our own world, settled and alive, with either lock held BbcoopSoloLockMs: both
// are released.
void ReleaseBbscLeaveLocks(std::string_view why);
constexpr s64 BbcoopSoloLockMs = 8'000;

void TickBbcoopSoloLeaveLocks() {
    static s64 since_ms = 0;
    static s64 last_ms = 0;
    const s64 now = SteadyNowMs();
    if (image_base == 0 || now - last_ms < 1'000) {
        return;
    }
    last_ms = now;
    const u64 global = GetGlobalState();
    const u64 lua_man = ReadValue<u64>(image_base + 0x0553B0C8, 0);
    const u64 lua = ReadablePointer(lua_man, 0x10) ? ReadValue<u64>(lua_man, 8) : 0;
    if (bbsc_v8_pending || GetSessionState() != 0 || !BbscOwnWorldNonGuest(global) ||
        ReadValue<u8>(global, 0x08) != 0 || GetBbscLocalPlayerHp() <= 0 ||
        ReadValue<u64>(image_base + WorldFlagManPointerOffset, 0) == 0) {
        since_ms = 0;
        return;
    }
    const bool leaving = BbscEventFlag(4047);
    const bool wait = ReadablePointer(lua, 0x90) && ReadValue<u8>(lua, 0x8D) != 0;
    if (!leaving && !wait) {
        since_ms = 0;
        return;
    }
    if (since_ms == 0) {
        since_ms = now;
        return;
    }
    if (now - since_ms < BbcoopSoloLockMs) {
        return;
    }
    since_ms = 0;
    ReleaseBbscLeaveLocks("alone in our own world, still locked");
    BBSC_TIMELINE("REPAIR", "alone in our own world with the leave locks on (flag 4047 {}, load wait {}) "
                            "- released", leaving, wait);
}

// ---- A kit bell used again stops it (2026-10-25) ---------------------------------------------------
// Asad: ring a bell and stop it. A vanilla bell, while its effect is on, is shown and used as its
// "silence" form (EquipParamGoods 200 -> 201 by SpEffect 9000, 205 -> 206 by 9005, 225 -> 226 by
// 9025). The kit rows have no such form (see "The BBSC hunter's kit"), so a ringing kit bell could
// not be stopped: the Parting Blank leaves a Hunt Bell's search on, and using the bell again only
// rang it again (test 2026-10-25, inventory). The vanilla forms used through the item use do not
// stop it either (test: 201 cost 1 Insight and the search went on). What ends a bell is its effect
// going - as when another bell is rung. So a kit bell used again while it has rung for
// BbcoopBellStopAfterMs has its effect taken off (BbscEraseLocalEffect); the game's Insight refund
// that follows is taken back by TickBbscFreeBellRefund.
u64 BbscLocalEffectNode(s32 id);
bool BbscEraseLocalEffect(s32 id);
struct BbcoopBellStop {
    s32 bell;
    s32 effect;
};
constexpr BbcoopBellStop BbcoopBellStops[] = {{4016, 9000}, {4121, 9005}, {11201, 9025}};
constexpr s64 BbcoopBellStopAfterMs = 1'500;
std::array<s64, std::size(BbcoopBellStops)> bbcoop_bell_on_since{}; // game thread
std::atomic<s32> bbcoop_bell_stop_effect{0};
bool bbcoop_auto_bell_paused = false; // see "Fast join"

// Item use (TraceEntry): a kit bell that already rings is to be stopped.
void NoteBbcoopBellUse(s32 goods) {
    const s64 now = SteadyNowMs();
    for (size_t i = 0; i < std::size(BbcoopBellStops); ++i) {
        if (goods == BbcoopBellStops[i].bell && bbcoop_bell_on_since[i] != 0 &&
            now - bbcoop_bell_on_since[i] >= BbcoopBellStopAfterMs) {
            bbcoop_bell_stop_effect.store(BbcoopBellStops[i].effect);
        }
    }
}

void TickBbcoopBellStop() {
    if (image_base == 0) {
        return;
    }
    const s64 now = SteadyNowMs();
    for (size_t i = 0; i < std::size(BbcoopBellStops); ++i) {
        const bool on = BbscLocalEffectNode(BbcoopBellStops[i].effect) != 0;
        bbcoop_bell_on_since[i] = on ? (bbcoop_bell_on_since[i] != 0 ? bbcoop_bell_on_since[i] : now) : 0;
    }
    const s32 effect = bbcoop_bell_stop_effect.exchange(0);
    if (effect == 0) {
        return;
    }
    const bool stopped = BbscEraseLocalEffect(effect);
    bbcoop_auto_bell_paused = true; // stopped by hand: the bells do not ring themselves until a load
    LOG_INFO(Debug, "bbcoop bells: a ringing kit bell used again - its SpEffect {} taken off ({})", effect,
             stopped ? "stopped" : "STILL ON");
    BBSC_TIMELINE("BELL", "a ringing kit bell used again - its SpEffect {} taken off ({})", effect,
                  stopped ? "stopped" : "STILL ON");
}

// ---- Fast join: the bells ring themselves (2026-10-25) ----------------------------------------------
// Asad picked one-press join: the launcher's Host or Join (BB_COOP_ROLE=host|join) is all a player
// does. Whenever the character stands settled and alive in its own world outside a boss fight, the
// host's Hunt Bell (search, SpEffect 9000) or the guest's Kin Bell (sign, 9005) rings by itself,
// again after a session ends or the game is restarted - that is the rejoin. A host rings while it
// has room for another hunter; a guest only while alone. Not while any bell already rings, and not
// after the player stopped one by hand (until the next load). The ring is the bell's effect added
// the way Lua_MultiDoping adds 7500 (as TickBbscInvadable adds 9020): no animation, no item.
// BB_COOP_AUTO_BELL=0: off.
constexpr s64 BbcoopAutoBellSettleMs = 5'000;
constexpr s64 BbcoopAutoBellRetryMs = 20'000;
constexpr s32 BbcoopAutoBellMaxHunters = 4;

void TickBbcoopAutoBell() {
    static const char* const role = [] {
        const char* value = std::getenv("BB_COOP_ROLE");
        const char* auto_bell = std::getenv("BB_COOP_AUTO_BELL");
        if (value == nullptr || (auto_bell != nullptr && auto_bell[0] == '0')) {
            return static_cast<const char*>(nullptr);
        }
        return std::string_view(value) == "host" ? "host"
               : std::string_view(value) == "join" ? "join"
                                                   : static_cast<const char*>(nullptr);
    }();
    static s64 last_ms = 0;
    static s64 settled_since_ms = 0;
    static s64 rung_ms = 0;
    if (role == nullptr || image_base == 0) {
        return;
    }
    const s64 now = SteadyNowMs();
    if (now - last_ms < 1'000) {
        return;
    }
    last_ms = now;
    const bool host = role[0] == 'h';
    const s32 state = GetSessionState();
    const u64 global = GetGlobalState();
    const u64 player = GetLocalPlayerCharacter();
    const bool loading = !ReadablePointer(global, 0x1598) || ReadValue<u8>(global, 0x08) != 0;
    if (loading) {
        bbcoop_auto_bell_paused = false;
    }
    const bool ready = !loading && BbscOwnWorldNonGuest(global) && GetCharacterHp(player) > 0 &&
                       IsSprjLuaEventManReady() && !BossFightActive() &&
                       (state == 0 || (host && state == 3));
    if (!ready) {
        settled_since_ms = 0;
        return;
    }
    if (settled_since_ms == 0) {
        settled_since_ms = now;
    }
    if (now - settled_since_ms < BbcoopAutoBellSettleMs || now - rung_ms < BbcoopAutoBellRetryMs ||
        bbcoop_auto_bell_paused) {
        return;
    }
    for (const s32 id : {9000, 9005, 9025}) {
        if (BbscLocalEffectNode(id) != 0) {
            return; // a bell rings already
        }
    }
    const s32 members = GetBbscSessionMemberCount(global);
    if (host && state == 3 && members >= BbcoopAutoBellMaxHunters) {
        return; // full
    }
    const u64 vtable = ReadValue<u64>(player, 0);
    const u64 add = ReadablePointer(vtable, 0x3F8) ? ReadValue<u64>(vtable, 0x3F0) : 0;
    if (!IsEbootCodeAddress(add)) {
        return;
    }
    const s32 effect = host ? 9000 : 9005;
    using AddSpEffect = void PS4_SYSV_ABI (*)(u64 chr, s32 id, u64 source, s32, s32, s32, float, float,
                                              float, float, float, s32);
    reinterpret_cast<AddSpEffect>(add)(player, effect, player, 0, 0, 0, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0);
    rung_ms = now;
    const bool on = BbscLocalEffectNode(effect) != 0;
    LOG_INFO(Debug, "bbcoop fast join: the {} rings by itself (SpEffect {} {}, session state {}, {} hunter(s))",
             host ? "Hunt Bell" : "Kin Bell", effect, on ? "on" : "NOT ON", state, members);
    BBSC_TIMELINE("AUTOBELL", "the {} rings by itself (SpEffect {} {}, session state {}, {} hunter(s))",
                  host ? "Hunt Bell" : "Kin Bell", effect, on ? "on" : "NOT ON", state, members);
}

// ---- BB_TEST_BELLS (2026-10-25) --------------------------------------------------------------------
// Test cheat for the free bell refund (TickBbscFreeBellRefund; throwaway profiles only). Once the
// character stands settled and alone in its own world, each kit bell in turn - Hunt, Kin, Bloodlust -
// is rung with the game's own item use (0x018F9720, as the responder resume rings its bell) and
// stopped 8 s later: `again` by using it again (as a player stops it), `death` by dying while it
// rings (a load in between), `both` a round of each. The next bell waits 25 s after the stop, past the refund
// window. Each step is a TEST line in the timeline; the INSIGHT lines show any net gain.
void TickBbcoopTestBells() {
    struct Step {
        s32 bell;
        bool death;
    };
    static const std::vector<Step> steps = [] {
        const char* value = std::getenv("BB_TEST_BELLS");
        const std::string mode = value != nullptr ? value : "";
        std::vector<Step> out;
        for (const bool death : {false, true}) {
            if (mode == "both" || mode == (death ? "death" : "again")) {
                for (const s32 bell : {4016, 4121, 11201}) {
                    out.push_back({bell, death});
                }
            }
        }
        return out;
    }();
    static size_t index = 0;
    static int phase = 0; // 0 settling, 1 ringing, 2 stopped
    static s64 since_ms = 0;
    if (index >= steps.size() || image_base == 0) {
        return;
    }
    const s64 now = SteadyNowMs();
    const Step& step = steps[index];
    const u64 global = GetGlobalState();
    const bool ready = GetSessionState() == 0 && ReadablePointer(global, 0x1598) &&
                       ReadValue<u8>(global, 0x08) == 0 && BbscOwnWorldNonGuest(global) &&
                       GetBbscLocalPlayerHp() > 0 && IsSprjLuaEventManReady();
    using UseItem = bool PS4_SYSV_ABI (*)(u64 player, s32 goods_id, s32 argument);
    const auto use_item = reinterpret_cast<UseItem>(image_base + UseItemNativeApplyOffset);
    if (phase == 1) {
        if (now - since_ms < 8'000) {
            return;
        }
        const bool sign = BbscLocalEffectNode(9005) != 0 || BbscLocalEffectNode(9025) != 0;
        const bool search = BbscLocalEffectNode(9000) != 0;
        if (step.death) {
            const u64 modules = ReadValue<u64>(GetLocalPlayerCharacter(), 0x3B0);
            const u64 data = ReadablePointer(modules, 0x28) ? ReadValue<u64>(modules, 0x20) : 0;
            if (ReadablePointer(data, 0xFC)) {
                WriteValue<s32>(data, 0xF8, 0);
            }
        } else {
            use_item(GetLocalPlayer(), step.bell, BellUseArgument);
        }
        BBSC_TIMELINE("TEST", "test bells: goods {} stopped by {} (session state {}, search effect {}, "
                              "sign effect {})",
                      step.bell, step.death ? "dying" : "using it again", GetSessionState(), search,
                      sign);
        phase = 2;
        since_ms = 0;
        return;
    }
    if (!ready) {
        since_ms = 0;
        return;
    }
    if (since_ms == 0) {
        since_ms = now;
    }
    if (now - since_ms < (phase == 0 ? 10'000 : 25'000)) {
        return;
    }
    if (phase == 2) {
        ++index;
        phase = 0;
        since_ms = now;
        BBSC_TIMELINE("TEST", "test bells: {} of {} done", index, steps.size());
        return;
    }
    const bool rung = use_item(GetLocalPlayer(), step.bell, BellUseArgument);
    BBSC_TIMELINE("TEST", "test bells: goods {} rung (item use {}), {} next", step.bell, rung,
                  step.death ? "a death" : "using it again");
    phase = 1;
    since_ms = now;
}
