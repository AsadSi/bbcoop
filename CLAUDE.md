# bbcoop

A private fork of bbport (Bloodborne running natively on Linux) that adds seamless co-op, co-op
tuning and respec. Read `docs/brief.md` first. It has the decisions, what's out of scope and
what's still open.

## Remotes
- Commits stay local for now; don't push (Asad, 2026-10-11).
- `origin` is AsadSi/bbcoop, private. Friends who get a build get the source (GPL-2.0+).
- `upstream` is deadinside28/bloodborne_pc, which changes daily. Merge `upstream/master`
  often.

## Keep rebases cheap
- New work goes in new files named `src/runtime_*.c`, which `build.sh` already compiles
  without any edits: `runtime_seamless.c` (13 byte patches, `BB_SEAMLESS=1`),
  `runtime_seamless_hooks.c` (shadp2p's 5 functional code hooks, on `runtime_hooks.c`),
  `runtime_respec.c` (the Doll's respec and weapon refund, 7 hooks, on by default;
  `docs/respec_re.md`), `runtime_camera.c` (the camera at uncapped frame rates, `docs/camera_re.md`),
  `runtime_ik.c` (the foot-IK height correction at uncapped frame rates, `docs/animation_re.md`),
  and later `runtime_coop.c`. Data mods go in
  `mods/coop-rebalance/`. The respec talk-script and text mod is generated from the player's own game
  files by `tools/respec_mod.py` (soulstruct, venv `~/.local/share/bbcoop-re`), which run.sh calls,
  into `mods/bbcoop-respec/` (git-ignored).
- Edits to upstream files stay small and marked: `probe.c`, `runtime_services.c`, `build.sh`,
  `launcher/`.
- Never reformat upstream code.

## Game patches
- Every co-op write to the game image checks the exact original bytes of every site first, and
  writes nothing if any of them differ. `src/runtime_seamless.c` shows the pattern, and
  `tests/test_seamless.c` is its test.
- Don't add co-op patches to `patches/*.xml`. That loader writes without checking.
- Addresses are image offsets: an XML `Address` minus 0x400000. That's the same convention as
  the `*Offset` constants in shadp2p's `src/core/bloodborne_re.cpp`.
- Everything is for v1.09: CUSA03173 (EU) or CUSA00900 (US). Their eboot.bin images are
  byte-identical (same `SUPPORTED_IMAGE` hash), so one set of offsets serves both.
  `scripts/game_check.py` rejects any other build.

## Online (M2): parked (2026-10-11)
- Online and co-op are on hold. Since the merge of upstream 0.51, online play is upstream's own
  module, `gpu/bbnet` (`libbbnet.so`, `BB_ONLINE=1`; `src/runtime_net.c` is upstream's).
- Our vendored shadp2p libraries are gone from the tree. Before the merge they were under
  `gpu/shadps4/core/libraries/{network,np}`, `gpu/shadps4/shadnet` and `gpu/shim/bbcoop_network.cpp`, with
  `tests/online_client.cpp` and `scripts/test-online.sh`; they are kept at the tag `bbcoop-online-0.4`.
  What has to be ported into `gpu/bbnet` is listed in `docs/brief.md` (Open).
- Kept: `server/` (the shadnet-p2p server, standalone), `launcher/bbcoop_online.py` (not loaded by
  the launcher), and `gpu/shim/bbcoop_placement.*` with `gpu/shim/core/bloodborne_re.h`, which the
  seamless hooks use.
- Seamless co-op is BBSC (`BB_BBSC=1`, Asad 2026-10-17), with bbcoop's scaling numbers (run.sh). `BB_SEAMLESS=1`
  (bbcoop's 13 byte patches and 5 hooks) is the fallback and is skipped while BBSC is on (same sites).
- Online servers: public ones are allowed (Asad, 2026-10-12, replacing "private only"): upstream's defaults
  (since 0.6 Seamless Hunt's, BBSC's; srv.shadps4.net refuses non-shadPS4 builds) or a group's own shadNet
  server (`server/`), in any mix.
- Respec: `BB_RESPEC=0` (no hooks), `BB_RESPEC_FREE=1` (stat reset costs no Insight),
  `BB_RESPEC_TRACE=1` (player state and hook calls in the log), `BB_TEST_ENDGAME=1` (cheat: a
  level-150 character with upgraded weapons, warped to the Dream with the Doll awake, once per save;
  only for throwaway profiles: `BB_USER_DIR` under out/, tools/new_game.sh), `BB_TEST_WARP=<lamp>` (cheat: once in
  game, to a lamp: dream, cathedral-ward, central-yharnam, old-yharnam or a respawn point ID <map><block>29xx;
  with tools/new_game.sh --continue it starts a test mid-game), `BB_GUN_GEMS=0` (the respec
  mod leaves out the gem slot it gives every firearm).
- Camera and frame rate: `BB_CAMERA_ACCEL=1` keeps the game's turn ramp (off by default), `BB_CAMERA_RATES=0`
  leaves the 30 FPS chase rates per frame, `BB_WALL_CLOCK=1` serves the game's `gettimeofday` from the wall
  clock instead of the monotonic one (`src/runtime_time.c`), and `BB_FPS_LIMIT` limits frames per second.
  Uncapped defaults to 120 (`run.sh`; the launcher's 0 keeps that default), `BB_FPS_LIMIT=0` means no limit.
  Uncap FPS++ is ps4_cheats' version of 2026-10-10 (timestep fixes, Sprint Fix built in); `BB_FPS_PATCH=old` selects
  the previous one, and `BB_FPS=120` applies 120 FPS++ (`docs/animation_re.md`).
  `BB_CAMERA_TRACE=1` logs the camera's frame time, rates, ramp and yaw. `BB_MESSENGER_RADIUS=<metres>` changes the messengers' 5 m emerge radius (`src/runtime_messenger.c`,
  `docs/messengers_re.md`).
  `BB_IK_HEIGHT` (`src/runtime_ik.c`, `docs/animation_re.md`): characters' foot-IK height correction gains
  follow the frame time by default; `0` or `off` turns the correction off (diagnostic), `vanilla` keeps the per-frame gains.
- Tests on the Mac: `scripts/docker-test.sh` runs the x86-64 build and C tests.

## References (siblings, read-only, shallow clones)
- `../shadp2p-ref` is shadPS4 with Bloodborne co-op. See `src/core/bloodborne_re.cpp`,
  `src/core/libraries/np/`, `documents/bloodborne-seamless-re.md` and
  `documents/README-bloodborne-internals.md`.
- `../shadnet-p2p-ref` is its matchmaking server, in C++.

## Running
- The game runs on x86-64 Linux (and on macOS, below) with Asad's own decrypted v1.09 dump. Use
  `./build.sh`, then `./run.sh`.
- `./build.sh --test` runs the C tests. The Python tests are in `tests/test_*.py`.
- macOS (Apple Silicon): upstream's port (bbport `macos-0.6`: PR 191 on 0.6, MetalFX; its `docs/MACOS.md`),
  an x86-64 build that Rosetta 2 runs, with MoltenVK. `scripts/mac-deps.sh` builds the x86-64 libraries into
  `out/mac-deps` (build.sh takes it, or `BB_MACOS_DEPS`). bbcoop's own Mac bits are marked `bbcoop` (hook
  entry symbol, BSD `stat` in run.sh, Rosetta's Intel CPU in patches.py, tools/new_game.sh without setsid).
- Never commit game files, dumps or saves.
- Driving the game (Steam Deck, KDE): `tools/new_game.sh` (clinic profile), `--continue` with
  `BB_SNAPSHOT=out/<profile>`, `tools/press.sh` for pad input, and `tools/frame.sh` (or `shot.sh`) for
  a screenshot from the game's own frame dump. That needs `BB_PRESENT_DUMP_TRIGGER=$PWD/out/present_trigger
  BB_DUMP_DIR=$PWD/out/present` at launch; spectacle cannot capture the game. Run everything inside
  `distrobox enter bbcoop`.
