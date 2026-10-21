# bbcoop on Windows

Since 2026-10-21 (Asad: cross-play tests on Linux, macOS and Windows). A native 64-bit build, built
in MSYS2 CLANG64 on Windows. It plays online with Linux and macOS bbcoop: everyone is on the shadPS4 0.20
module (gen 20).

## Where it comes from
- **Upstream 0.6:** the Win32 layer (`gpu/platform/win32/`, a POSIX subset over Win32) and the `WIN32`
  branch of `gpu/CMakeLists.txt` (a static libbbgpu linked into `bb-probe.exe` with the runtime).
- **Supermedo/bloodborne_pc** (branch `windows`, `de2e38a`, release 1.9.2): the Windows build of bbport.
  Its tree is older than 0.6's layout, so its pieces were copied in, not merged:
  - the `MINGW*|MSYS*` branch of `build.sh` and `msys2-env.sh`;
  - `run.py`, rewritten as a port of bbcoop's own `run.sh`, with Supermedo's Windows helpers;
  - the mod links in `scripts/mods.py`: files are hard links, folders are symlinks or junctions;
  - the red-zone fix in `scripts/link_modules.py` with `patches/redzone.json`;
  - `gpu/third_party/wepoll/`;
  - `launcher/bbport_launcher_win.py` with `bbport_lang*.py`, `bbport_steam.py` and the icons;
  - `packaging/windows/`.
- **bbcoop's own changes:**
  - `gpu/bbnet20` is built static on Windows and linked into `bb-probe.exe` (`gpu/CMakeLists.txt`,
    `gpu/bbnet20/CMakeLists.txt`). `src/runtime_net.c` takes its entry points directly there: as a DLL it
    would carry its own copy of the static GPU library.
  - Gen 19 (`gpu/bbnet`) is not built on Windows.

## Removed from Supermedo's launcher
These features have no runtime code in bbcoop:
- trophies;
- control remapping (`input.ini`);
- the cheats and tweaks pages;
- field of view, HUD position, frame generation, button prompts, mute in background;
- Quit to Desktop, the DLC folder, voice language;
- the SuperMedo Server pool and The Hunter's Dream live map;
- self-update from Supermedo's releases, which would replace bbcoop with bbport.

The launcher's pools match the Linux launcher's: Seamless Hunt (BBSC, `online_bbsc`) and The Hunter's
Dream. Its settings live in `%APPDATA%\bbcoop-launcher`.

## Red zone
Windows writes an exception's frame just below `rsp`, over the 128-byte System V red zone where the
game's leaf functions keep live locals. A guest store that faults on a write-protected page, then
resumes, finds them overwritten (Supermedo saw NULL loads in the attack-trail writer, 0x28ce7b0).
`link_modules.py` redirects 607 store sites to copies that run with `rsp` 128 bytes lower. Each site is
checked against its original bytes, and the copies go in a new executable segment.

The fix is on only on Windows (`BB_RED_ZONE=0/1` overrides). Checked on the Mac on 2026-10-21:
- All 607 sites match the 1.09 image.
- With the fix off, the link is byte-identical to before.
- No site comes within 32 bytes of a hook or patch constant in bbcoop's runtime, the guest hooks or
  BBSC, or within 16 bytes of any of the XML patches' 3,884 addresses.
- With `BB_RED_ZONE=1` the game loaded into the clinic with every hook installed (respec 9, save slots 3,
  camera, papers, all guest hooks).

## Build (on the Windows PC)
Install [MSYS2](https://www.msys2.org) (`winget install MSYS2.MSYS2`) and a python.org Python 3.12+ for
the package. Then, in the **MSYS2 CLANG64** shell:

```bash
pacman -S --needed git patch \
  mingw-w64-clang-x86_64-{toolchain,cmake,ninja,pkgconf,python} \
  mingw-w64-clang-x86_64-{vulkan-headers,vulkan-loader,vulkan-memory-allocator,glslang} \
  mingw-w64-clang-x86_64-{sdl3,fmt,boost,robin-map,xxhash,ffmpeg,zydis} \
  mingw-w64-clang-x86_64-{spirv-tools,spirv-headers,spirv-cross} \
  mingw-w64-clang-x86_64-{protobuf,openssl,zlib,miniupnpc,nlohmann-json}
# magic_enum and miniz come through CMake FetchContent when MSYS2 has no package (gpu/CMakeLists.txt).
git clone --recursive -b windows git@github.com:AsadSi/bbcoop.git && cd bbcoop
bash build.sh                       # out/bb-probe.exe, bb-gpu-capabilities.exe, bb-play.exe
BB_GAME_DIR='D:/path/to/CUSA03173' python run.py
python launcher/bbport_launcher_win.py
bash packaging/windows/package.sh   # dist/bbcoop-windows.zip
```

The package freezes the launcher with PyInstaller into `Bloodborne.exe`, with soulstruct for the respec
mod. Players need no Python. `run.py` takes the same variables as `run.sh`. From source, the respec mod
needs soulstruct in the Python that runs `run.py`, or in `BB_RE_PYTHON`.

## Open
- None of this has been built on Windows yet. Expect compile fixes, and keep them on the `windows` branch:
  - the force-included `bbport_win32.h` meets `play.c`, `wepoll.c` and bbcoop's own `src/runtime_*.c`;
  - bbnet20 has never been compiled for Windows;
  - Supermedo's tree has Windows fixes that 0.6's layer may not (`src/compat_win.c`, 207 commits on top of
    bbport); copy them in as each problem shows up.
- The C tests (`build.sh --test`) have no Windows path.
- Cross-play is not tested: Windows↔Mac and Windows↔Linux, with the Windows PC hosting and joining.
