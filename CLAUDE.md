# bbcoop

A private fork of bbport (deadinside28/bloodborne_pc: Bloodborne's own x86-64 code running natively on
Linux, with the PS4 system libraries and GPU replaced) that adds seamless co-op, co-op tuning, respec and
quality-of-life mods, and plays on macOS. Read `docs/bbcoop/brief.md` first: decisions, scope, what's open.

## Where bbcoop's code lives
Everything else in the tree is upstream's. bbcoop's own files sit beside it so merges stay cheap:

| What | Where |
|---|---|
| Our docs (brief, RE notes, Metal plan) | `docs/bbcoop/` (upstream's docs are the rest of `docs/`) |
| Runtime mods, one file each, compiled by `build.sh` without edits | `src/runtime_*.c` below |
| Co-op rules on BBSC (upstream's online modules) | `gpu/bbnet/shadps4/core/bbcoop_*.inc` and `gpu/bbnet20/bbsc/core/bbcoop_*.inc`, `include/core/bbcoop_loot.h` in each |
| Seamless placement and RE constants | `gpu/shim/bbcoop_placement.*`, `gpu/shim/core/bloodborne_re.h` |
| Respec talk-script/text mod generator | `tools/respec_mod.py` → `mods/bbcoop-respec/` (git-ignored, from the player's own files) |
| Our shadNet server (optional, standalone) | `server/` |
| Mac build pieces | `scripts/mac-deps.sh`, `scripts/mac/` (MoltenVK and mimalloc patches) |
| Windows build pieces | `docs/bbcoop/windows.md`, `run.py`, `msys2-env.sh`, `launcher/bbport_launcher_win.py`, `packaging/windows/` |
| Docker tests, server image | `scripts/docker-test.sh`, `scripts/docker-shell.nix`, `scripts/build-server.sh`, `scripts/server.Dockerfile` |
| Tests | `tests/test_*.c`, `tests/test_*.py` |

Runtime mods (`src/`):

| File | Does | Doc | Switches |
|---|---|---|---|
| `runtime_seamless.c` | 13 byte patches, seamless co-op fallback | | `BB_SEAMLESS=1` |
| `runtime_seamless_hooks.c` | shadp2p's 5 code hooks (on `runtime_hooks.c`) | | with `BB_SEAMLESS` |
| `runtime_respec.c`, `runtime_respec_rules.c` | the Doll's respec, workbench weapon refund (7 hooks, on) | `respec_re.md` | below |
| `runtime_camera.c` | camera at uncapped frame rates | `camera_re.md` | below |
| `runtime_ik.c` | foot-IK height correction at uncapped rates | `animation_re.md` | `BB_IK_HEIGHT` |
| `runtime_saveslots.c` | Duplicate in the Load Game list (Triangle) | `saveslots_re.md` | `BB_SAVESLOTS=0` off |
| `runtime_papers.c` | Fire/Bolt Paper, Empty Phantasm Shell scale with weapon and attack | `papers_re.md` | `BB_PAPERS=vanilla` off, `BB_PAPERS_TRACE=1` |
| `runtime_padmotion.c` | DualShock gyro/accelerometer for scePadReadState (from `runtime_pad.c`) | `padmotion_re.md` | `BB_PAD_MOTION=0` off, `BB_PAD_MOTION_TRACE=1` |
| `runtime_messenger.c` | messengers' 5 m emerge radius (diagnostic) | `messengers_re.md` | `BB_MESSENGER_RADIUS=<m>` |
| `runtime_context_lock.c` | load-race lock ("PM4 type 0"), from Wooloomooloo2 | `brief.md` | |

## Rules
- **Remotes:** commits stay local; don't push (Asad, 2026-10-11). `origin` is AsadSi/bbcoop, private; friends
  who get a build get the source (GPL-2.0+). `upstream` changes daily: merge it often.
- **Keep rebases cheap:** new work goes in new files (`src/runtime_*.c`, `docs/bbcoop/`). Edits to upstream
  files stay small and marked `bbcoop` (`probe.c`, `runtime_services.c`, `build.sh`, `run.sh`, `launcher/`).
  Never reformat or move upstream code.
- **Game patches:** every write to the game image checks the exact original bytes of every site first and
  writes nothing if any differ (`src/runtime_seamless.c`, test `tests/test_seamless.c`). No co-op patches in
  `patches/*.xml`: that loader writes unchecked. Addresses are image offsets (XML `Address` − 0x400000, as
  shadp2p's `*Offset` constants).
- **One game build:** v1.09, CUSA03173 (EU) or CUSA00900 (US), byte-identical eboots (same `SUPPORTED_IMAGE`
  hash). `scripts/game_check.py` rejects anything else. Never commit game files, dumps or saves.

## Online and co-op
- Online is upstream's module: since 0.6 (merged 2026-10-19) `gpu/bbnet20` (`libbbnet20`, shadPS4 0.20, the
  default), `BB_NET_GEN=19` the older `gpu/bbnet` (not on Windows); `BB_ONLINE=1`, `src/runtime_net.c` is
  upstream's. bbcoop's BBSC changes are in both, kept the same (`bbsc/core/` in bbnet20, `shadps4/core/` in bbnet;
  packets 0x23..0x28 in bbnet20's `np/signaling_handler.cpp`). BBSC's memory search reads AMD's dma-buf guest
  memory through `/proc/self/mem` (`bbsc_memscan.cpp`). `BB_GPU_LOG=info` shows BBSC's info lines.
- Seamless co-op is BBSC (`BB_BBSC=1`, Asad 2026-10-17) with bbcoop's scaling numbers (`run.sh`).
  `BB_SEAMLESS=1` is the fallback, skipped while BBSC is on (same sites).
- bbcoop's rules on BBSC: a guest's death at the host's lamp (`SHADPS4_BBSC_DEATH_TO_HOST_LAMP=0` off, test cheat
  `BB_TEST_GUEST_DEATH=<respawn point>`, `coop_respawn_re.md`), mirrored loot (`coop_loot_re.md`).
- Servers: public ones are allowed (Asad, 2026-10-12): upstream's defaults (Seamless Hunt's; srv.shadps4.net refuses
  non-shadPS4 builds) or a group's own shadNet server (`server/`), in any mix.
- Old vendored shadp2p networking is at tag `bbcoop-online-0.4`. `launcher/bbcoop_online.py` (Host/Join tab) is not
  loaded; porting it onto `gpu/bbnet` is open in the brief.

## Switches
- **Respec:** `BB_RESPEC=0` (no hooks), `BB_RESPEC_FREE=1` (no Insight cost), `BB_RESPEC_TRACE=1`, `BB_GUN_GEMS=0`
  (no gem slot added to firearms).
- **Test cheats** (throwaway profiles only: `BB_USER_DIR` under `out/`, `tools/new_game.sh`):
  - `BB_TEST_ENDGAME=1`: level-150 character, upgraded weapons, warped to the Dream with the Doll awake, once per save.
  - `BB_TEST_WARP=<lamp>`: once in game, to `dream`, `cathedral-ward`, `central-yharnam`, `old-yharnam` or a respawn
    point ID `<map><block>29xx`; with `tools/new_game.sh --continue` it starts a test mid-game.
  - `BB_TEST_GIVE=<id>,..`: weapons (final ids, `27001000` = Rakuyo +10) the character doesn't hold yet, on any save,
    saves backed up first.
  - `BB_TEST_LOADOUT=<list>`: fills a character named TEST once (stats 99, every weapon +10 Uncanny/Lost, all armor,
    stacks, gems); list `mods/bbcoop-respec/test-loadout.txt`; launcher `~/.local/bin/bloodborne-testchar.sh`.
- **Frame rate and camera:**
  - `BB_FPS`: `uncap` (Linux default), `30` (game's own), `60`/`90`/`120` (frame-rate hooks, capped). `BB_FPS_LIMIT`
    caps uncapped (default 120, `0` = none; the launcher's 0 keeps the default).
  - Uncap FPS++ is ps4_cheats' 2026-10-10 version; `BB_FPS_PATCH=old` the previous one; `BB_FPS=120` with
    `BB_FRAME_HOOKS=0` applies 120 FPS++ (`animation_re.md`).
  - A/B (`scripts/patches.py`, "Foliage speed" in `animation_re.md`): `BB_FPS_SKIP=<group>,..` (`cloth grass sfx fps60
    hknp timeobj clip`), `BB_FPS_TRY=<variant>,..` (`step1972 wind30 wind60 clothstep dtfloor`; default
    `wind60,clothstep,dtfloor`, Asad 2026-10-12; `none` = as published). `~/.local/bin/bloodborne-ab.sh ENV=VALUE...`.
  - `BB_CAMERA_ACCEL=1` keeps the game's turn ramp; `BB_CAMERA_RATES=0` keeps 30 FPS chase rates per frame;
    `BB_CAMERA_TRACE=1` logs them. `BB_WALL_CLOCK=1` serves `gettimeofday` from the wall clock (`src/runtime_time.c`).
  - `BB_IK_HEIGHT`: `0`/`off` (diagnostic), `vanilla` (per-frame gains); default follows the frame time.

## macOS (Apple Silicon)
- Upstream's Mac port (bbport `macos-0.6`: PR 191 on 0.6; its `docs/MACOS.md`): an x86-64 build run by Rosetta 2,
  Vulkan through our MoltenVK. `scripts/mac-deps.sh` builds the x86-64 libraries into `out/mac-deps` (`build.sh`
  takes it, or `BB_MACOS_DEPS`). bbcoop's Mac bits are marked `bbcoop` (hook entry symbol, BSD `stat` in run.sh,
  Rosetta's Intel CPU in patches.py, `tools/new_game.sh` without setsid).
- Our MoltenVK: repo `../moltenvk-bb` (branch `bbport`), shipped as `scripts/mac/moltenvk-bbport.patch`. Switches:
  `MVK_BB_HOIST_TESS` (fast path, flickers only the mist: run.sh turns it off with `BB_MIST=1`; off is ~15% slower), `MVK_BB_PASS_MERGE`,
  `MVK_BB_PASS_STATS=1`, `MVK_BB_UNTRACKED=1`. After a MoltenVK change: commit there, regenerate the patch, rerun
  `scripts/mac-deps.sh`.
- The Mac is GPU-bound by the game's effects, not by Rosetta. On an M4 Air the Central Yharnam street (test warp
  `BB_TEST_WARP=2412951`; `central-yharnam` is the clinic sickroom, indoors) runs ~55 FPS uncapped at 720p with every
  effect off and ~44 with every effect on. The native arm64 GPU process (branch `mac-gpu-process`) measured 1.05x and
  is parked.
- Asad plays locked 30 (Asad, 2026-10-19): 720p native, every effect on but SSR, model_lod 0 (`bbport.ini`).
  At 30 each frame is timed to the display's refreshes (`gpu/vulkan/swapchain.cpp` present times,
  `gpu/platform/videoout/display.cpp` vblank phase; `BB_PRESENT_PACE=0`, `BB_VBLANK_LOCK=0` off); `BB_FRAME_STATS=1`
  prints for how many refreshes frames stay on screen.
- No mist by default (`BB_MIST=1` keeps it): run.sh skips its two pixel shaders through `BB_SKIP_PS`; `BB_DRAW_LIST=<file>`
  lists a few frames' draws (pixel shader, primitive, blending) to find others. Lean texture usages (flicker only the mist: off with `BB_MIST=1`; `BB_FULL_IMAGE_USAGE=1` off, `BB_PROMOTE_LOG=1`), pipeline QoS (`BB_PIPELINE_QOS=0` off).

## Windows
- A native build in MSYS2 CLANG64 (`docs/bbcoop/windows.md`: packages, build, package, what came from where):
  upstream 0.6's Win32 layer plus Supermedo/bloodborne_pc's Windows build step, red-zone fix, mod links,
  launcher and package. `bash build.sh` there builds `out/bb-probe.exe`; `run.py` is the Windows `run.sh` and
  must carry the same bbcoop defaults (change both together); `bash packaging/windows/package.sh` makes
  `dist/bbcoop-windows.zip` (`Bloodborne.exe`, no Python needed).
- The online module (gen 20) is linked into `bb-probe.exe` there (`src/runtime_net.c`, `gpu/bbnet20/CMakeLists.txt`).
- Red-zone sites (`patches/redzone.json`, `scripts/link_modules.py`) are on only on Windows (`BB_RED_ZONE=0/1`).
- From the Mac: `bash scripts/windows/cross-build.sh` (llvm-mingw + MSYS2 packages) checks that the Windows
  executables still compile and link. Run it after changes that touch Windows code or after an upstream merge.

## Running and testing
- `./build.sh`, then `./run.sh` (or `python3 launcher/bbport_launcher.py --play`), with your own decrypted v1.09 dump.
- `./build.sh --test`: C tests. Python: `cd tests && ROSETTA_ADVERTISE_AVX=1 python3 -m unittest discover -p "test_*.py"`
  (the variable only matters on the Mac). On the Mac, `scripts/docker-test.sh` runs the Linux x86-64 build and C tests.
- Driving the game for tests: `tools/new_game.sh` (clinic profile; `--continue` with `BB_SNAPSHOT=out/<profile>`),
  `tools/press.sh` or `BB_PAD_FILE` for pad input, `BB_AUDIO=none` to mute, and `tools/frame.sh` / `shot.sh` for a
  screenshot from the game's frame dump (needs `BB_PRESENT_DUMP_TRIGGER=$PWD/out/present_trigger
  BB_DUMP_DIR=$PWD/out/present` at launch). On the Steam Deck run everything inside `distrobox enter bbcoop`.

## References (siblings, read-only)
- `../shadp2p-ref`: shadPS4 with Bloodborne co-op (`src/core/bloodborne_re.cpp`, `src/core/libraries/np/`,
  `documents/bloodborne-seamless-re.md`, `documents/README-bloodborne-internals.md`).
- `../shadnet-p2p-ref`: its matchmaking server (C++).
- `../bloodborne_mac_port-ref`: Wooloomooloo2's Mac port (GPL-2.0; credit him in files taken from it).
