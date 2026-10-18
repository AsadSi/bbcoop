// SPDX-License-Identifier: GPL-2.0-or-later
// bbcoop: mirrored loot (docs/bbcoop/brief.md, Loot: "every pickup, enemy drop and boss drop is mirrored to
// each player"). Included by bloodborne_re.cpp after its internal namespace, so it uses BBSC's
// helpers; declared in core/bbcoop_loot.h. Addresses and how they were found: docs/bbcoop/coop_loot_re.md.
//
// Send: every item get of the local player goes through the game's ItemGet (0x017D89F0: inventory,
// storage when full, the popup). A hook just after its prologue reads the item list and, from the
// return addresses, where the items came from. Mirrored: a map item taken with the pickup action
// (a world treasure, or an enemy's drop - kind 2), a pickup the host's game hands a guest, and an
// item lot the event script (2003[04]) awards when it is one of BBSC's boss reward lots. Not
// mirrored: everything else - NPC and quest event rewards, Lua's co-op/PvP mission rewards
// (GetRateItem, BlockClear2), shops, crafting, the Doll's refunds, BBSC's own gives and the items
// given here. The list goes out as relay packet "SHAD" 0x28 to every BBSC peer, sent three times
// (UDP), with a sender id and sequence number the receiver drops repeats by.
//
// Receive: the packet is queued on the signaling thread; the game tick gives the items through the
// same ItemGet with a list built the way the item lot award builds it (weapons, armor and gems as
// new instances), so the receiver gets the popup and the storage overflow of a real pickup, and no
// event flag of its own world is touched. A guest drops mirrored boss drops while BBSC's guest items
// are on: BBSC already gives the guest that boss's reward itself. Insight and echo consumables
// (Madman's Knowledge, Coldblood Dew...) are mirrored as items like any other pickup; nothing is
// used up for the receiver.
//
// SHADPS4_BBSC_MIRROR_LOOT=0 turns it off (and BBSC's own per-guest roll of enemy drops back on, as
// does a hook that did not go in).
// Test seam: SHADPS4_BBSC_MIRROR_LOOT_INJECT=<file>, polled once a second: each line
// "<item code> <count> [source]" becomes a packet from a made-up peer and goes the receive way.

namespace {

constexpr u64 ItemGetHookOffset = 0x017D8A04; // ItemGet 0x017D89F0 after its pushes
constexpr std::array<u8, 10> ItemGetHookBytes{0x49, 0x89, 0xF7,                   // MOV R15, RSI
                                              0x48, 0x89, 0xBD, 0x68, 0xFE, 0xFF, 0xFF}; // MOV [RBP-0x198], RDI
constexpr u64 ItemGetOffset = 0x017D89F0;     // (item manager, list)
constexpr u64 TakeItemReturn1 = 0x017D8758;   // the pickup action's take 0x017D8570, map item in R13
constexpr u64 TakeItemReturn2 = 0x017D88D3;
constexpr u64 HandedItemReturn = 0x017D3E8C;  // item packet handler 0x017D2F30: the host's answer
constexpr u64 AwardLotReturn = 0x017DE33B;    // item lot award 0x017DDC50 (AwardItemLotOffset)
constexpr u64 EventAwardReturn = 0x017C180B;  // event instruction 2003[04] in 0x017C0420
constexpr u64 ItemInstancesPointerOffset = 0x0553E990;
constexpr u64 WeaponGenerateOffset = 0x01A876D0; // (out, instances, doping, modify, features, id)
constexpr u64 ArmorGenerateOffset = 0x01A87390;  // (out, instances, id)
constexpr u64 GemGenerateOffset = 0x01A88130;    // (out, instances, doping, modify, features, id)
constexpr u64 HandleReleaseOffset = 0x01A8BC00;
constexpr size_t LootItemsMax = 6; // an item lot's and a map item's six slots
constexpr s64 LootApplyGapMs = 300;
constexpr std::array<s64, 2> LootResendMs{300, 1'500};

enum class LootSource : u8 { Pickup = 1, EnemyDrop = 2, BossDrop = 3, HandedPickup = 4, Test = 5 };

std::string_view LootSourceName(u8 source) {
    switch (static_cast<LootSource>(source)) {
    case LootSource::Pickup:
        return "pickup";
    case LootSource::EnemyDrop:
        return "enemy drop";
    case LootSource::BossDrop:
        return "boss drop";
    case LootSource::HandedPickup:
        return "pickup handed over by the host";
    case LootSource::Test:
        return "test";
    }
    return "unknown";
}

#pragma pack(push, 1)
struct BbcoopLootPacket {
    u8 magic[4] = {'S', 'H', 'A', 'D'};
    u8 type = kBbcoopLootPacketType;
    u8 version = 1;
    u8 source = 0;   // LootSource
    u8 count = 0;    // items, 1..6
    u32 sender = 0;  // the sending game's id (random per run)
    u32 sequence = 0;
    s32 lot = -1;    // the boss's item lot, else -1
    u32 map = 0;     // packed map it was obtained in (for the log)
    struct {
        u32 code; // category << 28 | id: weapon 0, armor 1, ring 2, goods 4, gem 8 (generator id)
        u32 count;
    } items[LootItemsMax]{};
};
static_assert(sizeof(BbcoopLootPacket) == 72);
#pragma pack(pop)

struct BbcoopLootOut {
    BbcoopLootPacket packet;
    size_t resent = 0;
    s64 sent_ms = 0;
};

thread_local bool bbcoop_loot_giving = false; // our own ItemGet runs: not sent on
std::mutex bbcoop_loot_mutex;
std::vector<BbcoopLootOut> bbcoop_loot_outbox;
std::vector<BbcoopLootPacket> bbcoop_loot_inbox;
std::vector<u64> bbcoop_loot_seen; // sender << 32 | sequence of packets taken
std::atomic<u32> bbcoop_loot_sequence{0};
std::atomic<bool> bbcoop_loot_hooked{false}; // the switch is on and the hook is in

u32 BbcoopLootSender() {
    static const u32 id = static_cast<u32>(
        static_cast<u64>(std::chrono::system_clock::now().time_since_epoch().count()) *
        2654435761ull >> 16) | 1u;
    return id;
}

std::string LootItemsText(const BbcoopLootPacket& packet) {
    std::string text;
    for (size_t i = 0; i < packet.count && i < LootItemsMax; ++i) {
        text += fmt::format("{}{:#x} x{}", i == 0 ? "" : ", ", packet.items[i].code,
                            packet.items[i].count);
    }
    return text;
}

bool IsBbscBossRewardLot(s32 lot) {
    return std::ranges::any_of(BbscBossRewards, [lot](const auto& r) {
        return lot > 0 && (r.first_lot == lot || r.repeat_lot == lot);
    });
}

// The game's stacks (runtime_low_map) are not in the memory map ReadValue checks; the frames and the
// list ItemGet is about to use are read as they are.
template <typename T>
T FrameValue(u64 address) {
    T value{};
    if (address >= 0x10000) {
        std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    }
    return value;
}

void PS4_SYSV_ABI BbcoopLootItemGetHook(u64 /*tag*/, const GuestRegisterSnapshot* registers) {
    if (registers == nullptr) {
        return;
    }
    if (bbcoop_loot_giving) {
        LOG_INFO(Debug, "bbcoop loot: ItemGet ran for our own give ({} item(s), first {:#x}) - not "
                        "sent on",
                 FrameValue<u32>(registers->rsi), FrameValue<u32>(registers->rsi + 8));
        return;
    }
    const u64 back = FrameValue<u64>(registers->rbp + 8);
    if (!IsEbootCodeAddress(back)) {
        return;
    }
    const u64 site = back - image_base;
    BbcoopLootPacket packet;
    if (site == TakeItemReturn1 || site == TakeItemReturn2) {
        // The map item (+0x20: its number << 4 | kind; kind 2 is what an enemy drops, 0x017DD0B0).
        const u32 item = ReadValue<u32>(registers->r13, 0x20);
        packet.source = static_cast<u8>((item & 0xF) == 2 ? LootSource::EnemyDrop : LootSource::Pickup);
    } else if (site == HandedItemReturn) {
        packet.source = static_cast<u8>(LootSource::HandedPickup);
    } else if (site == AwardLotReturn) {
        const u64 award = FrameValue<u64>(registers->rbp); // the award's frame
        const u64 award_back = FrameValue<u64>(award + 8);
        packet.lot = FrameValue<s32>(award - 0x1A4);
        if (award_back != image_base + EventAwardReturn || !IsBbscBossRewardLot(packet.lot)) {
            LOG_INFO(Debug, "bbcoop loot: item lot {} (award called from {:#x}) is not a pickup, "
                            "drop or boss drop - not mirrored",
                     packet.lot, IsEbootCodeAddress(award_back) ? award_back - image_base : 0);
            return;
        }
        packet.source = static_cast<u8>(LootSource::BossDrop);
    } else {
        return; // another item get (none known besides these)
    }
    const u64 list = registers->rsi;
    const u32 count = std::min<u32>(FrameValue<u32>(list), LootItemsMax);
    for (u32 i = 0; i < count; ++i) {
        const u32 code = FrameValue<u32>(list + 4 + i * 16 + 4);
        const u32 quantity = FrameValue<u32>(list + 4 + i * 16 + 8);
        if (code == 0xFFFFFFFFu || quantity == 0) {
            continue;
        }
        packet.items[packet.count++] = {code, quantity};
    }
    if (packet.count == 0) {
        return;
    }
    packet.sender = BbcoopLootSender();
    packet.sequence = ++bbcoop_loot_sequence;
    packet.map = GetCurrentPackedMap();
    const u32 peers =
        Libraries::Np::NpSignaling::Helpers::SendBbscWorldFlagsToPeers(&packet, sizeof(packet));
    {
        std::scoped_lock lock{bbcoop_loot_mutex};
        bbcoop_loot_outbox.push_back({packet, 0, SteadyNowMs()});
    }
    LOG_INFO(Debug, "bbcoop loot: {} [{}] lot {} map {:#x} - packet {} sent to {} peer(s)",
             LootSourceName(packet.source), LootItemsText(packet), packet.lot, packet.map,
             packet.sequence, peers);
}

// UDP: each packet goes twice more; the receiver keeps the first.
void ResendBbcoopLoot(s64 now) {
    std::vector<BbcoopLootPacket> due;
    {
        std::scoped_lock lock{bbcoop_loot_mutex};
        for (BbcoopLootOut& out : bbcoop_loot_outbox) {
            if (out.resent < LootResendMs.size() && now - out.sent_ms >= LootResendMs[out.resent]) {
                ++out.resent;
                due.push_back(out.packet);
            }
        }
        std::erase_if(bbcoop_loot_outbox,
                      [](const BbcoopLootOut& out) { return out.resent >= LootResendMs.size(); });
    }
    for (const BbcoopLootPacket& packet : due) {
        Libraries::Np::NpSignaling::Helpers::SendBbscWorldFlagsToPeers(&packet, sizeof(packet));
    }
}

// A new handle for the item, made as the item lot roll 0x01A8B080 makes it; 0 when it cannot be.
u32 MakeBbcoopLootHandle(u32 code) {
    const u32 id = code & 0x0FFFFFFFu;
    const u64 instances = ReadValue<u64>(image_base + ItemInstancesPointerOffset, 0);
    struct ParamRef {
        u32 id = 0xFFFFFFFFu;
        u32 pad = 0;
        u64 row = 0;
    };
    ParamRef doping;
    ParamRef modify;
    alignas(16) std::array<u8, 0x48> features; // no chalice features: all -1, as the award's default
    features.fill(0xFF);
    u32 handle = 0;
    using Generate = void PS4_SYSV_ABI (*)(u32* out, u64 instances, ParamRef* doping,
                                           ParamRef* modify, void* features, s32 id);
    switch (code >> 28) {
    case 0x0: // weapon
        if (instances != 0) {
            reinterpret_cast<Generate>(image_base + WeaponGenerateOffset)(
                &handle, instances, &doping, &modify, features.data(), static_cast<s32>(id));
        }
        return handle;
    case 0x1: // armor
        if (instances != 0) {
            using GenerateArmor = void PS4_SYSV_ABI (*)(u32* out, u64 instances, s32 id);
            reinterpret_cast<GenerateArmor>(image_base + ArmorGenerateOffset)(&handle, instances,
                                                                              static_cast<s32>(id));
        }
        return handle;
    case 0x2: // ring: no instance
        return id < 0x7FFFFF ? 0xA0000000u | id : 0;
    case 0x4: // goods: no instance
        return id < 0x7FFFFF ? 0xB0000000u | id : 0;
    case 0x8: // gem: a new one from the same generator row
        if (instances != 0) {
            reinterpret_cast<Generate>(image_base + GemGenerateOffset)(
                &handle, instances, &doping, &modify, features.data(), static_cast<s32>(id));
        }
        return handle;
    default:
        return 0;
    }
}

s32 BbcoopGoodsCount(u32 code) {
    const u64 carried = BbscCarriedInventory();
    const s32 slot = carried != 0 ? BbscInventorySlot(carried, code) : -1;
    const u64 entry = slot >= 0 ? BbscInventorySlotEntry(carried, static_cast<u32>(slot)) : 0;
    return entry != 0 ? ReadValue<s32>(entry, 8) : 0;
}

bool BbcoopLootCanGive() {
    const u64 global = GetGlobalState();
    const u64 holder = ReadValue<u64>(image_base + 0x0553B130, 0);
    const u64 menu = ReadValue<u64>(image_base + MenuManPointerOffset, 0);
    return ReadablePointer(global, 0x10) && ReadValue<u8>(global, 0x08) == 0 && // not loading
           ReadablePointer(holder, 0x10) && ReadValue<u64>(holder, 0x08) != 0 &&
           ReadablePointer(ReadValue<u64>(image_base + ItemManPointerOffset, 0), 0x10) &&
           ReadValue<u64>(image_base + ItemInstancesPointerOffset, 0) != 0 &&
           GetCharacterHp(GetLocalPlayerCharacter()) > 0 &&
           // BlockClear2's input lock: an item popup shown under it never closed (BBSC, 2026-10-11)
           (!ReadablePointer(menu, 0x2F4) || ReadValue<u32>(menu, 0x2F0) == 0);
}

void GiveBbcoopLoot(const BbcoopLootPacket& packet) {
    if (packet.source == static_cast<u8>(LootSource::BossDrop) && GetSessionState() == 6 &&
        EnvFlagEnabled("SHADPS4_BBSC_GUEST_ITEMS")) {
        LOG_INFO(Debug, "bbcoop loot: boss drop [{}] (lot {}) not given - BBSC gives the guest "
                        "this boss's reward itself",
                 LootItemsText(packet), packet.lot);
        return;
    }
    // The list as the item lot award 0x017DDC50 builds it: count, six {handle, code, count, -1}.
    struct alignas(16) {
        u32 count = 0;
        struct {
            u32 handle, code, count, extra;
        } items[LootItemsMax]{};
        u32 tail[5] = {0xFFFFFFFFu, 0, 0, 0, 0xFFFFFFFFu};
    } list;
    std::array<s32, LootItemsMax> before{};
    for (size_t i = 0; i < packet.count && i < LootItemsMax; ++i) {
        const u32 code = packet.items[i].code;
        const u32 handle = packet.items[i].count != 0 ? MakeBbcoopLootHandle(code) : 0;
        if (handle == 0) {
            LOG_WARNING(Debug, "bbcoop loot: item {:#x} could not be made - skipped", code);
            continue;
        }
        before[list.count] = BbcoopGoodsCount(code);
        list.items[list.count++] = {handle, code, std::min<u32>(packet.items[i].count, 999), 0xFFFFFFFFu};
    }
    if (list.count == 0) {
        return;
    }
    const u64 item_man = ReadValue<u64>(image_base + ItemManPointerOffset, 0);
    bbcoop_loot_giving = true;
    using ItemGet = void PS4_SYSV_ABI (*)(u64 item_man, void* list);
    reinterpret_cast<ItemGet>(image_base + ItemGetOffset)(item_man, &list);
    bbcoop_loot_giving = false;
    std::string counts;
    for (u32 i = 0; i < list.count; ++i) {
        if ((list.items[i].code >> 28) == 0x4) {
            counts += fmt::format(" goods {:#x}: {} -> {} carried;", list.items[i].code, before[i],
                                  BbcoopGoodsCount(list.items[i].code));
        }
        using Release = void PS4_SYSV_ABI (*)(u32* handle);
        reinterpret_cast<Release>(image_base + HandleReleaseOffset)(&list.items[i].handle);
    }
    LOG_INFO(Debug, "bbcoop loot: given a {}'s {} [{}] (packet {} of {:08x}, map {:#x}){}",
             packet.source == static_cast<u8>(LootSource::Test) ? "test" : "co-op partner",
             LootSourceName(packet.source), LootItemsText(packet), packet.sequence, packet.sender,
             packet.map, counts);
}

// The test seam: SHADPS4_BBSC_MIRROR_LOOT_INJECT=<file>, one "<code> <count> [source]" per line.
void PollBbcoopLootInject(s64 now) {
    static const char* path = std::getenv("SHADPS4_BBSC_MIRROR_LOOT_INJECT");
    static s64 last_ms = 0;
    if (path == nullptr || path[0] == '\0' || now - last_ms < 1'000) {
        return;
    }
    last_ms = now;
    std::ifstream file(path);
    if (!file) {
        return;
    }
    std::vector<std::string> lines;
    for (std::string line; std::getline(file, line);) {
        lines.push_back(line);
    }
    file.close();
    std::error_code ec;
    std::filesystem::remove(path, ec);
    static u32 sequence = 0;
    for (const std::string& line : lines) {
        char* end = nullptr;
        const u32 code = static_cast<u32>(std::strtoul(line.c_str(), &end, 0));
        const u32 count = static_cast<u32>(std::strtoul(end, &end, 0));
        const u32 source = static_cast<u32>(std::strtoul(end, &end, 0));
        if (count == 0) {
            continue;
        }
        BbcoopLootPacket packet;
        packet.source = static_cast<u8>(source != 0 ? source : static_cast<u32>(LootSource::Test));
        packet.count = 1;
        packet.sender = 0x7E57u; // a made-up peer
        packet.sequence = ++sequence;
        packet.map = GetCurrentPackedMap();
        packet.items[0] = {code, count};
        LOG_INFO(Debug, "bbcoop loot: test packet [{}] injected from {}", LootItemsText(packet), path);
        OnBbcoopLootPacket(&packet, sizeof(packet));
    }
}

} // namespace

bool BbcoopLootMirroring() {
    return bbcoop_loot_hooked.load(std::memory_order_acquire);
}

void OnBbcoopLootPacket(const void* data, std::uint32_t size) {
    BbcoopLootPacket packet;
    if (!BbcoopLootMirroring() || data == nullptr || size != sizeof(packet)) {
        return;
    }
    std::memcpy(&packet, data, sizeof(packet));
    if (std::memcmp(packet.magic, "SHAD", 4) != 0 || packet.type != kBbcoopLootPacketType ||
        packet.version != 1 || packet.count == 0 || packet.count > LootItemsMax ||
        packet.sender == BbcoopLootSender()) {
        return;
    }
    const u64 key = u64{packet.sender} << 32 | packet.sequence;
    std::scoped_lock lock{bbcoop_loot_mutex};
    if (std::ranges::find(bbcoop_loot_seen, key) != bbcoop_loot_seen.end()) {
        return; // a resend of one already taken
    }
    bbcoop_loot_seen.push_back(key);
    if (bbcoop_loot_seen.size() > 256) {
        bbcoop_loot_seen.erase(bbcoop_loot_seen.begin());
    }
    if (bbcoop_loot_inbox.size() >= 64) {
        LOG_WARNING(Debug, "bbcoop loot: 64 mirrored item gets waiting - the oldest dropped");
        bbcoop_loot_inbox.erase(bbcoop_loot_inbox.begin());
    }
    bbcoop_loot_inbox.push_back(packet);
}

void InstallBbcoopLoot() {
    if (BbcoopLootMirroring() || image_base == 0) {
        return;
    }
    if (!EnvFlagEnabled("SHADPS4_BBSC_MIRROR_LOOT")) {
        LOG_INFO(Debug, "bbcoop loot: mirroring off (SHADPS4_BBSC_MIRROR_LOOT=0)");
        return;
    }
    const bool hooked =
        InstallGuestCodeHook(reinterpret_cast<void*>(image_base + ItemGetHookOffset),
                             std::span<const u8>{ItemGetHookBytes}, 0, BbcoopLootItemGetHook);
    bbcoop_loot_hooked.store(hooked, std::memory_order_release);
    if (hooked) {
        LOG_INFO(Debug, "bbcoop loot: mirroring pickups, enemy drops and boss drops (ItemGet hook "
                        "at {:#x}, sender {:08x})",
                 ItemGetHookOffset, BbcoopLootSender());
    } else {
        LOG_ERROR(Debug, "bbcoop loot: ItemGet hook not installed at {:#x} (bytes differ) - "
                         "nothing is mirrored",
                  ItemGetHookOffset);
    }
}

void TickBbcoopLoot() {
    if (!BbcoopLootMirroring()) {
        return;
    }
    static s64 last_given_ms = 0;
    const s64 now = SteadyNowMs();
    ResendBbcoopLoot(now);
    PollBbcoopLootInject(now);
    if (now - last_given_ms < LootApplyGapMs || !BbcoopLootCanGive()) {
        return;
    }
    BbcoopLootPacket packet;
    {
        std::scoped_lock lock{bbcoop_loot_mutex};
        if (bbcoop_loot_inbox.empty()) {
            return;
        }
        packet = bbcoop_loot_inbox.front();
        bbcoop_loot_inbox.erase(bbcoop_loot_inbox.begin());
    }
    last_given_ms = now;
    GiveBbcoopLoot(packet);
}
