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
  "pickup"; kind 2 is not mirrored (below). Kind 1 is ItemGet's
  own ground drop (`0x017DA960`, called at `0x017D95DC`): what the carry limit and storage didn't take.
  It is not mirrored (2026-10-24): a given copy that landed at the receiver's feet went back to the
  sender as a new pickup every time it was picked up.
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

Relay packet "SHAD" 0x28, 80 bytes, to every BBSC peer (`SendBbscWorldFlagsToPeers`):
`magic[4] "SHAD", u8 0x28, u8 version 2, u8 source (1 pickup, 2 enemy drop, 3 boss drop, 4 pickup
handed over by the host, 5 test), u8 count (1..6), u32 sender (random per game run), u32 sequence,
s32 lot (boss drop, else -1), u32 packed map, six {u32 code, u32 count}, then s32 treasure flag (the
MSB treasure's pickup flag, else -1) and u32 treasure item (its number << 4 | 3 in the sender's game,
for the log)`. Version 1 (2026-10-17..24, 72 bytes, no treasure fields) is still taken and given as a
plain pickup; a version 1 build drops version 2 packets (size check), so every hunter needs this build.
Each packet is sent
three times (now, after 300 ms and 1.5 s); the receiver keeps the first of each (sender, sequence).
The signaling receive thread (`np_signaling_helpers.cpp`) hands type 0x28 to `OnBbcoopLootPacket`.

## Rules

- **No loop**: the items ItemGet gives for a packet are never sent (a thread-local flag, and their
  caller is not one of the three above).
- **No double with BBSC's boss rewards**: a guest (session state 6) drops a mirrored boss drop while
  `SHADPS4_BBSC_GUEST_ITEMS` is on, because BBSC gives the guest that boss's reward itself (first or
  repeat lot, by the guest's own flag). BBSC's own gives are never sent. So with BBSC's guest items on,
  boss drops are not mirrored at all; with them off, the host's boss drop is.
- **Enemy drops are not mirrored (2026-10-24):** every hunter's game drops its own copy when an enemy
  dies, so mirroring one doubled it (seen in both players' logs). BBSC's own roll for a guest
  (`NoteBbscGuestEnemyDeath`) stays skipped while mirroring is on; it would be a third copy.
- **World state**: ItemGet sets no flag, so a mirrored item never marks the receiver's own pickup as
  taken, except a treasure of the session's world (below), whose copy the receiver takes away.
- **Insight and echo items** (Madman's Knowledge, Coldblood Dew and the like) are mirrored as items
  like any pickup; nothing is used up for the receiver, who decides whether to use them.
- **Key items** are mirrored too; one the receiver already holds is handled by the game's own add
  (the inventory's limits).

## Test seam

`SHADPS4_BBSC_MIRROR_LOOT_INJECT=<file>`: polled once a second; each line `<code> <count> [source]
[treasure flag]` (C numbers) becomes a packet from a made-up peer and takes the receive path; the file is deleted once
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

## Treasures: first grab shares (Asad, 2026-10-24)

In a BBSC session every hunter sees every treasure of the host's world: BBSC's guest items
(`SHADPS4_BBSC_GUEST_ITEMS`, on by BBSC's default again; `run.sh`/`run.py` no longer turn it off)
give each guest its own copy (GuestItems.SpawnOwn / CreateOwn / PlaceTreasures) next to the host's.
Whoever picks a treasure up first gets it, every other hunter gets a copy, and their glowing copy
goes away, taken in that world. Found 2026-10-24 in `out/eboot.elf` (objdump):

- **Identity: the pickup flag.** A map item is `+0x20` number << 4 | kind, `+0x28` pickup flag
  (ItemLotParam getItemFlagId, -1 none), `+0x48` flags (0x40 pickable, 0x80 treasure), `+0xB8` a
  second flag. The flag is the same treasure in every game; the number is not: each game numbers its
  items from its own counter (GuestItems.NumberKind*). BBSC's guest-items code pairs a guest's copy
  with the host's treasure by that flag too (kind 5 groups 5AABx, `ApplyBbscGuestLootView`,
  `LogBbscHostWorldFlagsDiff`: "taken by the host (still here for us)").
- **Send.** The take `0x017D8570` finds the item (`0x017D81A0`) and then removes it before ItemGet
  runs, so a hook at `0x017D85B0` (`41 0F B7 45 48 A8 40`, MOVZX EAX,[R13+0x48] / TEST AL,0x40; R13
  the item) keeps its number and flag for the ItemGet hook. A kind 3 item with a flag goes out with
  `treasure flag` set while the sender is the host (session state 3) or a guest in the host's world;
  the sender also remembers the flag (one list per game run).
- **Removal, as the game's take does it:** `0x017D6780(item man, &number, 1)` unlinks the item (its
  glow and effect go with it) and, with the 1, sets its `+0x28` and `+0xB8` bits in the flag table.
  A guest in the host's world skips that marking (`0x017D6ADC`), which GuestItems.MarkTaken jumps
  over, so the guest's flag is set too. `0x017DE6E0` is the same plus, on the host, packet 0x2C
  `{1, 2, number}` to every member; a guest's item manager (`0x017D2F30`, kind 2 in the jump table at
  `0x017D4C10`) calls `0x017D6780(.., number, 1)` at `0x017D34D1` for it.
- **Receive.** The receiver gives the copy (if it has the treasure already, nothing: see below), then
  looks for its own kind 3 item with that flag in the item manager's three lists and removes it with
  `0x017D6780(.., 1)`: on the host the flag stays set in its world like a pickup; on a guest BBSC's
  item capture then keeps it gone for that character (taken file, own world), as when the guest takes
  it itself. Not lying there (another block): the flag alone is set (`SetEventFlag 0x0132E5A0`).
  Only in the session's world: the host, or a guest in the host's world with guest items on.
- **Has it already: nothing given.** The flag is in the receiver's list (its own pickup took it this
  run, or another hunter's copy was given for it), or the receiver is the host and the flag is set
  (taken before the session). A guest's flag alone is no proof: the host's 0x2C kind 2 above sets it.

| Case | What happens |
|---|---|
| Host first | Host keeps its pickup; each guest gets a copy, its copy is removed and marked taken |
| Guest first | Guest keeps its pickup; the host gets a copy, the host's treasure is removed and its flag set in the host's world; other guests the same |
| Both at once | Each keeps its own pickup and drops the other's packet (the flag is in its list): one each |
| Host took it before the session | The guest still sees its copy; when it takes it the host gets nothing (flag set) |
| Guest took it before (own world or another host's) | Its view hides the copy; when the host takes it the guest still gets a copy (its flag is no proof, above) |
| Guest's own world, or not in a session | No treasure flag is sent; a pickup there is mirrored as before and nothing is removed |

## Doubles (what can give an item twice)

- **A treasure in the host's world, in a BBSC session**: shared first grab (above). Left: a guest
  that took a treasure in an earlier run (its own world or another host's) gets a copy when the
  host takes it in this session.
- **The host's 0x2C kind 2 on a guest** (BBSC's, not new): the guest's game removes its item of the
  host's item number, which with independent numbering can be another treasure. Not changed.
- **The receiver's own world**: a mirrored plain pickup leaves the receiver's pickup flags alone
  (shared progress does not merge kind 5 either), so the receiver can take the same item again at
  home. A shared treasure's copy taken by a guest goes into its own world too (BBSC's capture).
- **Enemy drops**: each hunter takes the one its own game drops; none is mirrored.
- **Boss drops**: none in a BBSC session (the guest keeps BBSC's own reward and drops the mirrored
  one); with `SHADPS4_BBSC_GUEST_ITEMS=0` the guest gets the host's instead.
- **Overflow**: what ItemGet cannot fit is dropped on the ground (`0x017DA960`, kind 1); picking it
  up is not mirrored again.

## Not verified (needs a real session)

- A packet crossing the relay between two players, and its resends.
- Whether every guest is a signaling peer of every other guest (three or more hunters): a guest's
  pickup reaches only its established peers.
- The guest side of a pickup handed over by the host (`0x017D3E8C`).
- Boss drops end to end, and the guest's mirrored items while BBSC's guest-items view runs.
- Shared treasures (2026-10-24): built only. The take hook, the removal call from the game tick, the
  guest's flag through MarkTaken and the cases in the table above have not run in a game.

## Found on the way

The signaling receive thread handed on only BBSC's 0x20 and 0x22 packets; 0x23 (life), 0x24 (scaling),
0x25 (PvP), 0x26 (shared progress) and 0x27 (boss bar) were sent but ended in "unexpected SHAD packet",
also in upstream 0.6-pre1, so their `OnBbsc*Packet` handlers never ran. Fixed in bbcoop 120bf10
(`np_signaling_helpers.cpp`, each checked against the sender's packet size).
