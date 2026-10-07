# bbcoop — brief

Started 2026-10-07. Approved plan: `~/.claude/plans/i-was-considering-making-crystalline-reddy.md`.

## What this is

A private fork of bbport (deadinside28/bloodborne_pc) that adds seamless co-op for a small group of friends. One player's PC hosts the session and the others join over Tailscale. It also adds co-op enemy tuning, shared loot, and a respec option at the Doll that costs Insight.

**Done** means two friends on Linux PCs can:
- beat Father Gascoigne together without being sent home,
- travel by lamp together,
- both receive every pickup and boss drop,
- respec stats or refund a weapon's materials at the Doll.

## Not building

- Our own runtime or recompiler. bbport already runs the game's x86-64 code natively, and forking it gives us full control.
- A public server, matchmaking with strangers, or any connection to real PSN.
- PvP or invasions.
- Chalice Dungeon co-op.
- macOS or Windows builds.
- Support for any game version other than CUSA03173 v1.09.

## Decisions made

| Decision | Choice | Why | By |
|---|---|---|---|
| Base | Fork of bbport | Recompiling ourselves would rebuild bbport for no gain, since the mods need RE of game logic either way | Asad |
| Hosting | The host player's PC runs the server inside its launcher, and friends connect over Tailscale | No separate server to maintain | Asad |
| Audience | Private friend group, private GitHub repo | GPL: everyone who gets a build also gets the source | Asad |
| Party size | Host plus 2 guests (the game's own limit) | Going above the native limit means more RE and more desync | Asad |
| Guest death | Respawns at the host's last lamp and is re-inserted | Seamless Co-op model | Asad |
| Host death | The host respawns at their lamp and guests stay where they are, with no reset for them | Asad: the host dying must not send everyone back | Asad |
| Progress | Boss kills, lamps and shortcuts are copied into each guest's own save | Seamless Co-op model | Claude |
| Loot | Every pickup, enemy drop and boss drop is mirrored to each player | "Fair for co-op" | Asad |
| Difficulty | +50% HP and +10% damage per extra player, plus extra enemies in chosen spots during co-op only | | Asad |
| Respec | At the Doll, in the Hunter's Dream, outside a session. Costs Insight (1 per stat reset, 1 per weapon refund, pending confirmation). Saves a backup first | | Asad |
| Seamless patches | Go through `src/runtime_seamless.c`, which checks the original bytes, not bbport's XML loader | The XML path writes without checking, so a wrong address would silently corrupt the game | Claude |
| Our code | In new `src/runtime_*.c` files, which `build.sh` already compiles, with only small marked edits to upstream files | bbport changes every day, so rebasing must stay cheap | Claude |
| Addresses | shadp2p offsets are already bbport image offsets (an XML address minus 0x400000, as shown by `memory_patcher.cpp:282` and `scripts/patches.py:197`) | Checked 2026-10-07 | Claude |

## Reusing

- **bbport** (this repo) provides the loader, runtime, renderer, mod overlay (`scripts/mods.py`) and patch compiler (`scripts/patches.py`).
- **shadp2p** (`../shadp2p-ref`, GPL-2.0+):
  - `src/core/bloodborne_re.cpp` has the seamless patches with their original and new bytes, plus the native function offsets (lines 104–188).
  - `src/core/libraries/np/np_matching2`, `np_signaling`, `np_web_api*` and `src/shadnet/` are the NP client.
  - `documents/bloodborne-seamless-re.md` and `README-bloodborne-internals.md` are its RE notes.
- **shadnet-p2p** (`../shadnet-p2p-ref`, GPL-2.0+, C++) is the matchmaking, signaling and WebAPI server, to be embedded in host mode.
- **Not reusable:** `~/Documents/code/oot-recomp` (N64Recomp) targets a different CPU architecture and only overlaps in general RE habits.
- **Modding tools:** Smithbox, Paramdex BB, WitchyBND and DarkScript3 for the param, map, event-script and talk-script edits.

## Shape

There are three layers, each with one owner:
1. **Game networking:** `src/runtime_net.c` and `src/runtime_np.c`. These are the PS4 Net, NetCtl, Http and NP functions that the game calls, ported from shadp2p. The game's own netcode stays in charge of summoning, insertion and combat sync.
2. **Mod channel:** `src/runtime_coop.c`. Versioned messages, sent through the host, for what the game doesn't sync: lamp travel, event flags, item lots, echoes, deaths and enemy resets. This is the only place where co-op rules live.
3. **Data mod:** `mods/coop-rebalance/dvdroot_ps4/...`, loaded through bbport's existing loose-file overlay.

Supporting pieces:
- **Hooks:** `src/runtime_hooks.c` (M1) checks the expected bytes and installs absolute-jump trampolines into C handlers.
- **Respec:** in `src/runtime_coop.c`, triggered by an event flag that the Doll's talk script sets.

## Screens

- **Build now:** a launcher Host/Join panel. The host sees "share this IP and code"; a guest enters the host's IP and code.
- **Later:** an in-game session overlay showing who's connected.

## Open

- Insight costs: 1 per stat reset and 1 per weapon refund until Asad confirms. Blocks M6 tuning only.
- Weapon refund: whether gems in the weapon are removed or kept. Blocks M6.
- "Extra enemies in certain situations": which areas or bosses. Central Yharnam is the first template. Blocks M5 rollout.
- Everything that runs the game is blocked until Asad's Linux PC and v1.09 dump are available. The Mac is arm64 and can't run the game.
- shadp2p's room-retention patch at 0x019471B1 (the `StageTransitionKeepMatchingCall` bytes) is declared and its original bytes are checked, but nothing ever writes it in `bloodborne_re.cpp`. So it's left out of `runtime_seamless.c` until we know whether it's needed. This is part of M3 blocker 1.
