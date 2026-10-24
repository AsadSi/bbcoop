# A guest's death: respawn at the host's last lamp

Asad's rule (`docs/bbcoop/brief.md`, "Guest death"): a dead guest respawns at the host's last lamp and stays in the
session. The host's own death is unchanged (BBSC: the host respawns at its lamp and the guests stay).

Code: `gpu/bbnet/shadps4/core/bbcoop_coop.inc`, included into `bloodborne_re.cpp`'s anonymous namespace
right before `BbscRespawnInPlace` so it can use BBSC's helpers. The calls into it from BBSC's code are
marked `// bbcoop:`. All addresses are image offsets for v1.09.

## What BBSC already did

- **The host's lamp reaches the guest.** The host sends its last lamp (`global+0x1528`) with every follow
  packet, twice a second (`TickBbscFollowBroadcast`, `BbscFollowMessage::lamp_point`).
- **A death outside a boss fight, with a partner alive.** Here `TickBbscSpectate` takes the
  "no boss fight" branch, then the death curtain, then `BbscRespawnInPlace`. That revives the guest
  without a load, at the host's lamp, but only if the lamp is in a map loaded on the guest's side
  (`ResolveLampPoint`, 0x0154AFC0, finds points in loaded blocks only). Otherwise the guest goes
  beside the host, if the host's map is loaded here, or to the lamp of its own map nearest its body.
- **A boss fight.** The guest waits until the boss dies or nobody is left alive, then respawns the same
  way, or reloads with the host after a wipe.

## What bbcoop adds

If the host's lamp is not in a loaded map, the guest loads into that lamp's map. The game places the
guest at the lamp, and then BBSC's revive runs on the new character. By then the lamp is loaded, so
the guest stands at the host's lamp with full HP, and the game shows "You respawned at the host's
lamp."

- **When it applies.** Only for a guest in a session (state 6, in the host's world, not an invader), and
  only in `TickBbscSpectate`'s "no boss fight" branch (`BbscDeathMode::RespawnNow`).
- **Boss fights stay BBSC's.** The guest waits out the fight, and after the kill it respawns beside the
  host or at a loaded lamp. Asad's brief keeps BBSC's boss handling. After a kill the party is at the
  arena, and a load would split it right where the next lamp lights.
- **Falling back to BBSC's respawn.** BBSC's own respawn runs when:
  - the switch is off;
  - the host's lamp is unknown (no follow packet, or -1);
  - the point names no map (area 0, or 29 for the Chalice Dungeons);
  - a load is already running;
  - the load does not start within 10 s;
  - the session ends during the load.
  After 2 minutes BBSC's watchdog takes over in any case.
- **Switch.** `SHADPS4_BBSC_DEATH_TO_HOST_LAMP` is on by default with BBSC (`bbsc_persistence.h`
  `DefaultOnSwitches`); `=0` turns it off.

## Respawn in place and the full recover (2026-10-24)
- A death's reload runs the game's full recover: the death steps set `[0x0553B130]+0x70`
  (SoloPlayDeath_2 `0x01381EF9`, PartyGhostDeath_2 `0x013825ED`), and the player create `0x01939470`
  calls `0x018FD460(player)` (`0x019395CF`) before clearing the byte (`0x01939610`). It sets HP and
  stamina to max, the 5 status meters `[[chr+0x3B0]+0x28]+0x10..` back to max, and times out status
  SpEffects (stateInfo 2/5/6/0x74/0x75/0x88/0x104).
- `BbscRespawnInPlace` has no reload, and `TickBbscDeathLockClear` drops the byte, so poison stayed
  after a co-op respawn (host and guest). It now calls `0x018FD460` itself, after the death globals.
- Still not reset by design: enemies (BBSC: "the world is not reset"). Vials and bullets come from
  `BbscRestockFromStorage`.

## The load (CONFIRMED statically)

**The placement choice.** The stage flow builds a load's placement in `0x01943F10`, which checks in this
order:

| Check | Kind | Placement |
|---|---|---|
| event flag 6005, or the task's +0x9C | 8 | m24_01 |
| `+0x1523` | 10 | the Dream (point 0x2016B1) |
| `+0x1521` (summon placement) | 2 | position `+0x14D0`, rotation `+0x14E0` |
| `+0x1538 != 0` | 9 | point `+0x153C`, second value `+0x1540` |
| an event-flag case (around flag 8146; not needed here) | 6 | |
| `+0x1524` (lamp) | 3 | point `+0x1530`, or `+0x1528` if that is -1; second value `+0x1534` / `+0x152C`; map from the point's digits (0x01944E3C) |
| `+0x1520` (forced warp) | 4 | |
| `+0x15E4` | 5 | |

All offsets are from the game state `global` = `[0x05556678]`.

**Kind 9 is the respawn-point warp.** Event instruction 2003[49], `WarpToRespawnPoint` (0x013CDF30, which
bbcoop's `BB_TEST_WARP` calls), does this:
1. It looks up the warp id in the param table at 0x05320B40 (0x01F26B40). Row layout: +0 area (u16),
   +2 block (u16), +4 point id, +8 second value, +0xC lamp flag.
2. It sets `+0xC` = area<<24 | block<<16.
3. It sets `+0x1538` = 1 when the row's +0xC is set (lamp travel), else 2.
4. It sets `+0x153C/+0x1540` = (row+4, row+8), then calls StageTransition 0x013CDE30.

**After a mode-1 arrival, lamp travel's tail** (0x019423A0 at 0x01942491) sets `+0x1524 = 1` and
`+0x1528/+0x152C` = (row+4, row+8). So `+0x1528` holds the same kind of point id that `+0x153C` takes,
and that is what the host sends. Mode 2 leaves the last lamp alone.

**After any placement**, 0x0156D300 clears `+0x1538`, `+0x1524` and the dword `+0x1520..0x1523`.

**Map from a point id.** The lamp respawn computes it as area = id / 100000 % 100 and
block = id / 10000 % 10. For example, 2412959 is m24_01 and 2102961 is m21_00. The map files hold
these ids: m24_01 has 2412950..2412999, m24_02 has 2422950..2422993, and m21_00 has 2102950..2102962
(scanned from `map/MapStudio/*.msb.dcx`).

**What bbcoop writes**, then calls StageTransition:

| Field | Value | Why |
|---|---|---|
| `+0x1520` | 0 | no leftover forced warp; PartyGhostDeath_2 raises it to send the guest home |
| `+0x1521` | 0 | no summon placement |
| `+0x1523` | 0 | no Dream placement |
| `+0xC` | lamp map | |
| `+0x1538` | 2 | plain warp: the guest's own last lamp stays its own |
| `+0x153C` | host's lamp point | |
| `+0x1540` | the guest's own `+0x152C` | see below |

For a guest it also does what BBSC's `RequestBbscSessionWarp` does for its follow loads:
- `+0x14C4` = the lamp map;
- summon reload phase `[[0x0553D6D0]+0x84]` = 3, so the stage teardown 0x019470F0 keeps the session and
  the load step 0x019297E0 takes its map from `+0x14C4`; BBSC's at-load world-flag and loot hooks read
  `+0x14C4` too;
- the host's world flags for that map, if the host sent them;
- the guest loot view;
- the load cover, named "host lamp", which ends like BBSC's "wipe" cover.

**The second value (`+0x1540`).** It is the warp row's +8, which the placement passes as the arrival
setting (r9+0x84; the defaults there are animation ids 101200 and 101300). The host does not send its
`+0x152C`. The clinic profile's Central Yharnam lamp had 2412959 / 101163, and the Dream point set by a
first death had 0. bbcoop passes the guest's own value. This is HYPOTHESIS: the right value for every
lamp is unverified, and with 0 the game placed the character normally (test below).

**The death flags.** `+0x1522` (died) stays as the death set it. The placement copies it (r9+0x94), as for
the game's own death reload. `BbscRespawnInPlace` clears it after the load, together with the event
state, the death request (4) and event flag 4000.

## Detecting the load

`global+0x08` is StageTransition's request byte. The stage flow consumes it within moments, so a
once-per-tick check missed it in the first test. A load is now taken as any of:
- that byte;
- the loading screen (`[0x05562878]+0x804 != 0`);
- a new local character (`WorldChrMan+0x60`).

Once the new character has a transform and nothing is loading for 1 s, `BbscRespawnInPlace` runs.

## A defect found on the way: BBSC could not read the game's image

`gpu/bbnet/include/core/memory.h` answered BBSC's "is this mapped" from the runtime's map
(`runtime_memory_vma_info`), which holds only direct and flexible memory. The eboot image at
0x800000000 is host-owned low memory outside that map, and every BBSC global lives in it (WorldChrMan
0x0553E878, the game state 0x05556678, SprjLuaEventMan 0x0553B0C8). So every `ReadValue` of a game global
returned 0.

The evidence was a solo run in the clinic with `BB_GPU_LOG=info`:
- the player read as 0 and the Lua manager as 0 in the level (1200+ draws per frame);
- "BBSC session shape (tick)" stayed at state -1 / own_world -1.

The image's range (`bbnet_image_ready`: `MemoryPatcher::g_eboot_address`, `g_eboot_image_size`) now counts
as mapped read/write (commit 05df2e5, `memory.h` only).

## Testing

**The test seam.** `BB_TEST_GUEST_DEATH=<respawn point id>` is a cheat for throwaway profiles only:
1. Once in a level, the local player is killed once, after 10 s alive, through SetHp 0x018BCD40 with 0.
2. That solo death, from `BbscSpectateDeathHook`'s solo branch, is held like a guest's kept death.
3. A follow message is made up, as the host would send it, with the given point as the host's lamp.
4. `BbscRespawnInPlace` treats the player as a guest.
5. Everything from `BbcoopRespawnAtHostLamp` on is the real code. Only the phase-3/+0x14C4 session
   part, the world flags and the cover are guest-only, and they can be checked only in a real session.

**Running it.** Use `BB_ONLINE=1 BB_BBSC=1 BB_BBSC_LOCAL_SERVER=0 BB_AUDIO=none BB_GPU_LOG=info`, with the
clinic profile continued (Play Offline). The log lines start with `bbcoop test:` and `bbcoop death:`.

## With BBSC's rejoin (`SHADPS4_BBSC_REJOIN`, on by default)

At a guest's kept death, the death step's StageTransition call goes to BBSC's no-op. Its hook,
`ArmBbscRejoin`, then does two things in v7:
- sets summon reload phase 3;
- arms reinsert v8, which queues the guest's player type as the pending type for the next load
  (`[0x0553B130]+0x8C`) and rebuilds the remote players once the player is alive again.

BBSC's own respawn without a load leaves both waiting for the next load. bbcoop's load to the host's lamp
is that next load: it writes phase 3 again (the same value) and points `+0x14C4` at the lamp's map. So the
reload REJOIN prepared at the death becomes the load to the lamp. The guest stays a phantom (v8's type),
and the host's character is rebuilt after the load (v8's tick, 120 s limit).

With `SHADPS4_BBSC_REJOIN=0`, bbcoop sets phase 3 itself, as BBSC's follow loads do.

Reused from BBSC:
- the follow packet's host lamp;
- `ResolveLampPoint`;
- the death-penalty step, which runs before the load (`BbscApplyDeathPenalty`, `SHADPS4_BBSC_DEATH_PENALTY`);
- `RequestBbscSessionWarp`'s session-keeping writes;
- the world-flag and loot-view helpers, and their at-load hooks;
- the load cover;
- `BbscRespawnInPlace` for the revive and its cleanup;
- the spectate state, which holds the death, so BBSC's watchdog stays quiet.

## Verified in game (2026-10-17, macOS, solo, clinic profile, the test seam)

| Run | Host's lamp | Result |
|---|---|---|
| e | 2422950 (m24_02, not loaded in the clinic) | The game loaded m24_02 with its loading screen and placed the player at (-24.64, 40.62, 250.26). The lamp point resolves to (-24.64, 40.79, 250.26). `BbscRespawnInPlace` revived the player there (hp 594), and the screen showed "You respawned at the host's lamp." with the area title "Upper Cathedral Ward" and the lamp's "Light Lamp" prompt. The own last lamp was unchanged (mode 2). |
| f | 2412959 (m24_01, loaded) | No load. BBSC's curtain and respawn in place took the player to the lamp, as before. |
| g | 2422950 with `SHADPS4_BBSC_DEATH_TO_HOST_LAMP=0` | No load. BBSC's fallback took the player to the nearest lamp of the map (2412950). |
| b | 2102961 (the Dream) | The load ran, but the clinic profile's first death also makes the Dream its own respawn (the death step set `+0x1528` = 2102962). So the run proves little, and it showed the first version's load detection was wrong. |

Not verifiable alone (needs a real two-player session):
- the guest-only part: phase 3 and `+0x14C4` keeping the session through the load;
- REJOIN v8 rebuilding the host's character afterwards;
- the host's world flags for the lamp's map (sent only for the block the host stands in);
- the load cover ending;
- whether `TickBbscFollowHost` leaves the guest at the lamp. It should: the host did not move, so this
  counts as "we walked away" and there is no reload.
