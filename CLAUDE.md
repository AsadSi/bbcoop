# bbcoop

A private fork of bbport (Bloodborne running natively on Linux) that adds seamless co-op, co-op
tuning and respec. Read `docs/brief.md` first. It has the decisions, what's out of scope and
what's still open.

## Remotes
- `origin` is AsadSi/bbcoop, private. Friends who get a build get the source (GPL-2.0+).
- `upstream` is deadinside28/bloodborne_pc, which changes daily. Rebase onto `upstream/main`
  often.

## Keep rebases cheap
- New work goes in new files named `src/runtime_*.c`, which `build.sh` already compiles
  without any edits: `runtime_seamless.c` (byte patches, `BB_SEAMLESS=1`), and later
  `runtime_hooks.c`, `runtime_coop.c`, `runtime_net.c` and `runtime_np.c`. Data mods go in
  `mods/coop-rebalance/`.
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
- Everything is for CUSA03173 v1.09 only. `scripts/game_check.py` rejects any other build.

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
