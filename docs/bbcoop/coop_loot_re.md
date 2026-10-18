# Mirrored loot: reverse-engineering notes (v1.09)

Asad's decision (`docs/bbcoop/brief.md`, Loot): every pickup, enemy drop and boss drop is mirrored to each
player. Code: `gpu/bbnet/shadps4/core/bbcoop_loot.inc`, included by BBSC's `bloodborne_re.cpp`
(so it runs only with `BB_ONLINE=1 BB_BBSC=1`). Switch: `SHADPS4_BBSC_MIRROR_LOOT` (on by default
with BBSC, `=0` off). All addresses are image offsets. Found on 2026-10-17 by disassembling
`out/eboot.elf` (objdump, `.eh_frame_hdr` function bounds, direct-call and `lea` scans) and reading
`eventcommon.luabnd`; no public offsets were used.

## Every item get goes through ItemGet

`ItemGet(item man, list)` at `0x017D89F0` puts items into the inventory (AddItem `0x014CDA70` on
PGD+0x1D0), what does not fit into the storage (`0x014D9A80` / `0x014DA0A0`), what fits nowhere on the
ground (`0x017DA960`), and shows the item popup (`0x013107B0`). It sets no event flag. Its callers:

| Return address | Caller | What it is |
|---|---|---|
| `0x017D8758`, `0x017D88D3` | `0x017D8570` | The pickup action's take (`0x012FD880`): a map item; the map item is in R13 |
| `0x017D3E8C` | `0x017D2F30` | The item manager's packet handler: the items the host's game hands a guest for its pickup |
| `0x017DE33B` | `0x017DDC50` | The item lot award (BBSC's `AwardItemLotOffset`) |

The list: `u32 count`, then six 16-byte items `{u32 handle, u32 code, u32 count, u32 extra}` (the award
writes extra = -1), then `-1, lot, 0 (u64), -1`. ItemGet reads only `count` items. `code` is
`category << 28 | id` (weapon 0, armor 0x1, ring 0x2, goods 0x4, gem 0x8 with the GemGenParam id).
`handle` is what the inventory stores: `0xA0000000 | id` for a ring, `0xB0000000 | id` for goods,
an instance (bit 0x800000, `& 0xFFFF` indexes `[0x0553E990]`) for weapons, armor and gems.
`0x01A89070` turns a handle into its code (non-instance handles: bits 28-30 are 0 weapon, 1 armor,
2 ring, 3 and 4 goods).

The hook sits just after ItemGet's pushes, at `0x017D8A04` (`49 89 F7` MOV R15,RSI;
`48 89 BD 68 FE FF FF` MOV [RBP-0x198],RDI), byte-checked by `InstallGuestCodeHook`. BBSC's pickup
trace already hooks ItemGet's first 6 bytes. There RSI is the list, RBP is ItemGet's frame (`[RBP+8]`
its return address, `[RBP]` the caller's frame) and R13 still holds the caller's R13.

## Where the items came from

- **Map items** (pickups): `[R13+0x20]` is the item's number `<< 4 | kind`. The kind is the spawn
  descriptor's first field: the enemy drop `0x017DD0B0` writes 2 (`0x017DDA60`), the MSB treasure
  placement `0x017DC300` writes 3 (`0x017DCA56`; BBSC's log shows treasure 0x53). Kind 2 is sent as
  "enemy drop", anything else as "pickup"; the two are mirrored the same way.
- **Enemy drops are map items.** The death of an enemy runs `0x018C4F90(chr)`, which calls
  `0x017DD0B0(item man, chr id, NpcParam info)`. That rolls the enemy's item lot (`0x01A8B080`) and
  spawns the result as a kind 2 map item at the enemy (`0x017D5320`), which a player takes with the
  pickup action. The kill reward `0x018C5070` only pays echoes.
- **Item lot awards**, `0x017DDC50(item man, lot, respect multiplayer)`: the lot is the award's
  `[RBP-0x1A4]`, its return address the award's `[RBP+8]`. Its callers:

| Return | Caller | Mirrored |
|---|---|---|
| `0x017C180B` | the event interpreter `0x017C0420`, instruction 2003[04] | Only BBSC's boss reward lots (`BbscBossRewards`, first and repeat lots): boss drops. Other event lots are NPC and quest rewards |
| `0x0132E498`, `0x0132E4B5` | Lua `GetRateItem`, `GetRateItem_IgnoreMultiPlay` | No: `eventcommon.luabnd` calls them for co-op, PvP and invasion mission rewards (each player's own game runs them) |
| `0x0138xxxx` | Lua `DeadInfoMsg_Host`, `DeadInfoMsg_ForceJoinBlack`, `TextEffectEnd_PK_Success`, `TextEffectEnd_NPCPK_Success`, `BlockClear2` | No: multiplayer rewards |
| outside the eboot | BBSC (guest boss rewards, guest enemy rolls) | No |

  The Lua names come from the registration code (`0x0133B370`, `0x0138D400`: each `lea` of the function
  is followed by the `lea` of its name).
- Shops, crafting, the Doll's refunds (`AddItemOrStorage`), the respec test kit and other menus do not
  call ItemGet, and nothing else does: they are never sent.

## Giving the items

On the receiving side the game tick calls ItemGet itself with a list built the way the award builds
it, one handle per item, as the lot roll `0x01A8B080` makes them:

| Category | Handle |
|---|---|
| weapon | `0x01A876D0(&out, [0x0553E990], &doping, &modify, features, id)` |
| armor | `0x01A87390(&out, [0x0553E990], id)` |
| ring | `0xA0000000 \| id` |
| goods | `0xB0000000 \| id` |
| gem | `0x01A88130(&out, [0x0553E990], &doping, &modify, features, GemGenParam id)`: a new gem of the same row (its random effects are rolled again) |

`doping` and `modify` are `{u32 -1, u32 0, u64 0}` (no param, as `docs/bbcoop/respec_re.md` passes them);
`features` is 0x48 bytes of -1, the award's default when no chalice feature applies (`0x0556B888`).
After ItemGet the handles are released (`0x01A8BC00`); the inventory keeps its own reference.

It waits while loading (`[global]+0x08`), while the player is dead or missing, and while BlockClear2's
input lock `[0x05562878]+0x2F0` is set: BBSC saw an item popup shown under it stay on screen
(2026-10-11, "Sword Hunter Badge").

## The packet

Relay packet "SHAD" 0x28, 72 bytes, to every BBSC peer (`SendBbscWorldFlagsToPeers`):
`magic[4] "SHAD", u8 0x28, u8 version 1, u8 source (1 pickup, 2 enemy drop, 3 boss drop, 4 pickup
handed over by the host, 5 test), u8 count (1..6), u32 sender (random per game run), u32 sequence,
s32 lot (boss drop, else -1), u32 packed map, then six {u32 code, u32 count}`. Each packet is sent
three times (now, after 300 ms and 1.5 s); the receiver keeps the first of each (sender, sequence).
The signaling receive thread (`np_signaling_helpers.cpp`) hands type 0x28 to `OnBbcoopLootPacket`.

## Rules

- **No loop**: the items ItemGet gives for a packet are never sent (a thread-local flag, and their
  caller is not one of the three above).
- **No double with BBSC's boss rewards**: a guest (session state 6) drops a mirrored boss drop while
  `SHADPS4_BBSC_GUEST_ITEMS` is on, because BBSC gives the guest that boss's reward itself (first or
  repeat lot, by the guest's own flag). BBSC's own gives are never sent. So with BBSC's guest items on,
  boss drops are not mirrored at all; with them off, the host's boss drop is.
- **Enemy drops**: BBSC's own roll of each enemy for a guest (`NoteBbscGuestEnemyDeath`) is skipped
  while mirroring is on, because the drop a hunter picks up now reaches every other hunter. With
  `SHADPS4_BBSC_MIRROR_LOOT=0` it is back.
- **World state**: ItemGet sets no flag, so a mirrored item never marks the receiver's own pickup as
  taken. A player can therefore still take the same treasure in their own world (or, in a session,
  the copy BBSC gives every guest), and then everyone gets it again.
- **Insight and echo items** (Madman's Knowledge, Coldblood Dew and the like) are mirrored as items
  like any pickup; nothing is used up for the receiver, who decides whether to use them.
- **Key items** are mirrored too; one the receiver already holds is handled by the game's own add
  (the inventory's limits).

## Test seam

`SHADPS4_BBSC_MIRROR_LOOT_INJECT=<file>`: polled once a second; each line `<code> <count> [source]`
(C numbers) becomes a packet from a made-up peer and takes the receive path; the file is deleted once
read. The send side logs every packet with the number of peers it went to (0 when playing alone):
`bbcoop loot: pickup [0x400003e8 x1] lot -1 map ... sent to 0 peer(s)`.

## Verified in game (2026-10-17, Mac, clinic profile, `BB_ONLINE=1 BB_BBSC=1`, alone)

- The hook goes in: `bbcoop loot: mirroring pickups, enemy drops and boss drops (ItemGet hook at
  0x17d8a04, ...)`.
- Receive: an injected file with Blood Vial x3 (`0x400003e8`), Saw Cleaver (`0x006acfc0`, weapon
  7000000) and a gem from GemGenParam 110001 (`0x8001adb1`) gave all three through ItemGet:
  `goods 0x400003e8: 0 -> 3 carried`, the game's own "items obtained" popup listed Blood Vial 3, Saw
  Cleaver 1 and Tempering Blood Gemstone (1) 1, and the HUD's vial count went from 0 to 3. No fault.
- Before the image fix (05df2e5) every BBSC read of a game global returned 0 and the items waited in
  the queue; the game's stacks (`runtime_low_map`) are still outside the memory map ReadValue checks,
  which is why the hook reads its frames and the list directly.
- Not done: a real pickup. The glowing marks on the clinic's ground floor are tutorial messages, not
  items, and the first item is outside the clinic, past the beast. The send side's classification is
  static analysis only (above), and its log line was not seen in game.

## Doubles (what can give an item twice)

- **A treasure in the host's world, in a BBSC session**: BBSC gives each guest its own copy of every
  treasure (GuestItems.SpawnOwn / CreateOwn; the host's copy stays). Each copy taken is mirrored, so a
  treasure both hunters take reaches each of them twice.
- **The receiver's own world**: a mirrored pickup leaves the receiver's pickup flags alone (shared
  progress does not merge kind 5 either), so the receiver can take the same treasure again at home.
- **Enemy drops**: one per kill reaches everyone through whoever picks the drop up; BBSC's extra
  per-guest roll is off while mirroring runs. If a guest also sees and takes the host's drop as its
  own copy (not verified), that copy is mirrored too.
- **Boss drops**: none in a BBSC session (the guest keeps BBSC's own reward and drops the mirrored
  one); with `SHADPS4_BBSC_GUEST_ITEMS=0` the guest gets the host's.
- **Overflow**: what ItemGet cannot fit is dropped on the ground (`0x017DA960`); picking that up later
  is a new pickup and is mirrored again.

## Not verified (needs a real session)

- A packet crossing the relay between two players, and its resends.
- Whether every guest is a signaling peer of every other guest (three or more hunters): a guest's
  pickup reaches only its established peers.
- The guest side of a pickup handed over by the host (`0x017D3E8C`).
- Boss drops end to end, and the guest's mirrored items while BBSC's guest-items view runs.

## Found on the way

The signaling receive thread handed on only BBSC's 0x20 and 0x22 packets; 0x23 (life), 0x24 (scaling),
0x25 (PvP), 0x26 (shared progress) and 0x27 (boss bar) were sent but ended in "unexpected SHAD packet",
also in upstream 0.6-pre1, so their `OnBbsc*Packet` handlers never ran. Fixed in bbcoop 120bf10
(`np_signaling_helpers.cpp`, each checked against the sender's packet size).
