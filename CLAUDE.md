# bbcoop

A private fork of bbport (Bloodborne running natively on Linux) that adds seamless co-op, co-op
tuning and respec. Read `docs/brief.md` first. It has the decisions, what's out of scope and
what's still open.

## Remotes
- `origin` is AsadSi/bbcoop, private. Friends who get a build get the source (GPL-2.0+).
- `upstream` is deadinside28/bloodborne_pc, which changes daily. Merge `upstream/master`
  often.

## Keep rebases cheap
- New work goes in new files named `src/runtime_*.c`, which `build.sh` already compiles
  without any edits: `runtime_seamless.c` (13 byte patches, `BB_SEAMLESS=1`),
  `runtime_seamless_hooks.c` (shadp2p's 5 functional code hooks, on `runtime_hooks.c`) and
  `runtime_net.c`, `runtime_respec.c` (the Doll's respec and weapon refund, 7 hooks, on by default;
  `docs/respec_re.md`), and later `runtime_coop.c`. Data mods go in
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

## Online (M2)
- `gpu/shadps4/core/libraries/{network,np}` and `gpu/shadps4/shadnet` hold shadp2p's online
  libraries. They're mostly unmodified, and every edit is marked `bbcoop`. They're adapted
  through shims in `gpu/shim/` and registered by `gpu/shim/bbcoop_network.cpp`.
- `src/runtime_net.c` routes the game's network imports to them, but only when
  `BB_SHADNET_SERVER` is set.
- `server/` is the vendored shadnet-p2p server (Qt 6.9). Its FromSoft game-server routes are in
  `server/src/webapi_routes_fromsoft.cpp`.
- Environment variables:
  - `BB_SHADNET_SERVER=host[:31313]`
  - `BB_SHADNET_NPID`
  - `BB_SHADNET_PASSWORD`
  - `BB_SHADNET_INVITE`: the server's `RegistrationSecretKey`; it creates the account on
    first contact.
  - `BB_SHADNET_WEBAPI`: defaults to `http://host:31315`.
  - `BB_SEAMLESS=1`
  - `BB_CAPTURE_ONLINE=1`: captures every HTTP request.
- Respec: `BB_RESPEC=0` (no hooks), `BB_RESPEC_FREE=1` (stat reset costs no Insight),
  `BB_RESPEC_TRACE=1` (player state and hook calls in the log), `BB_TEST_ENDGAME=1` (cheat: a
  level-150 character with upgraded weapons, warped to the Dream with the Doll awake, once per save;
  only for throwaway profiles: `BB_USER_DIR` under out/, tools/new_game.sh).
- Private only: while online, HTTP to any host other than the shadNet server is refused, in
  `RunRealHttpRequest`. Never weaken this.
- Tests on the Mac:
  - `scripts/docker-test.sh` runs the x86-64 build and C tests.
  - `scripts/test-online.sh` is the end-to-end run, with the server plus host and guest
    `tests/online_client.cpp`.

## References (siblings, read-only, shallow clones)
- `../shadp2p-ref` is shadPS4 with Bloodborne co-op. See `src/core/bloodborne_re.cpp`,
  `src/core/libraries/np/`, `documents/bloodborne-seamless-re.md` and
  `documents/README-bloodborne-internals.md`.
- `../shadnet-p2p-ref` is its matchmaking server, in C++.

## Running
- The game only runs on x86-64 Linux with Asad's own decrypted v1.09 dump. Use `./build.sh`,
  then `./run.sh`.
- `./build.sh --test` runs the C tests. The Python tests are in `tests/test_*.py`.
- The Mac (arm64) can edit code and run the Python tests, but can't run the game.
- Never commit game files, dumps or saves.
- Driving the game (Steam Deck, KDE): `tools/new_game.sh` (clinic profile), `--continue` with
  `BB_SNAPSHOT=out/<profile>`, `tools/press.sh` for pad input, and `tools/frame.sh` (or `shot.sh`) for
  a screenshot from the game's own frame dump. That needs `BB_PRESENT_DUMP_TRIGGER=$PWD/out/present_trigger
  BB_DUMP_DIR=$PWD/out/present` at launch; spectacle cannot capture the game. Run everything inside
  `distrobox enter bbcoop`.
