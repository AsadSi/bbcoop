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
the package. The repo is private: clone it from PowerShell with Git for Windows (`winget install Git.Git`),
which signs in through the browser:

```powershell
git config --global core.longpaths true
git config --global core.autocrlf false   # LF endings: bash scripts and the FSR-Vulkan patches need them
git clone --recursive -b windows https://github.com/AsadSi/bbcoop.git C:\bbcoop
```

Then, in the **MSYS2 CLANG64** shell:

```bash
pacman -Syu   # once; if the window closes, open MSYS2 CLANG64 again and repeat
pacman -S --needed git patch \
  mingw-w64-clang-x86_64-{toolchain,cmake,ninja,pkgconf,python} \
  mingw-w64-clang-x86_64-{vulkan-headers,vulkan-loader,vulkan-memory-allocator,glslang} \
  mingw-w64-clang-x86_64-{sdl3,fmt,boost,robin-map,xxhash,ffmpeg,zydis} \
  mingw-w64-clang-x86_64-{spirv-tools,spirv-headers,spirv-cross} \
  mingw-w64-clang-x86_64-{protobuf,openssl,zlib,miniupnpc,nlohmann-json}
# magic_enum and miniz come through CMake FetchContent when MSYS2 has no package (gpu/CMakeLists.txt).
cd /c/bbcoop   # cloned from PowerShell above: it signs in to GitHub in the browser
bash build.sh                       # out/bb-probe.exe, bb-gpu-capabilities.exe, bb-play.exe
bash packaging/windows/package.sh   # dist/bbcoop-windows.zip (after the game runs)
```

To play from source, use the Windows Python in PowerShell. `run.py` adds `C:\msys64\clang64\bin` to `PATH`
for the DLLs:

```powershell
cd C:\bbcoop
py -3.12 -m pip install soulstruct==2.6.0      # the respec mod
py -3.12 launcher\bbport_launcher_win.py       # or: $env:BB_GAME_DIR="D:/Games/CUSA00900"; py -3.12 run.py
```

The package freezes the launcher with PyInstaller into `Bloodborne.exe`, with soulstruct for the respec
mod. Players need no Python. `run.py` takes the same variables as `run.sh`. From source, the respec mod
needs soulstruct in the Python that runs `run.py`, or in `BB_RE_PYTHON`.

## Checking from the Mac
`bash scripts/windows/cross-build.sh` builds `bb-probe.exe`, `bb-gpu-capabilities.exe` and `bb-play.exe` with
llvm-mingw against the MSYS2 CLANG64 packages, which it unpacks into `out/windows-cross/`. It takes about
1.5 minutes after the first download (1.4 GB). Run it after Windows-relevant changes or upstream merges. The
executables are for checking, not for players.

Fixed through it on 2026-10-21. Each fix is behind `_WIN32`/`WIN32` and marked `bbcoop`, and the Mac build,
tests and game are unchanged:
- xbyak: MSYS2 has no package, so it is fetched (v7.43, as `scripts/mac-deps.sh`).
- `bb_win_compat.h` duplicated the Win32 layer's `gettid`, so it is no longer force-included.
- wepoll's `sigset_t` clashed with `bbport_signal.h`'s.
- BBSC's memory search used SEH `__try`, which clang for MinGW lacks: now `ReadProcessMemory` in pieces.
- `setenv` became `_putenv_s`, with a narrow path string.
- Statically linked, names that each module keeps private on Linux (`-Bsymbolic`) collided:
  - bbnet20's `runtime_thread_spawn` forwarder is left out, so the runtime's own is used;
  - bbnet20's `SymbolsResolver` is renamed;
  - the GPU library's `Core::Bloodborne` placement functions, which only the online modules call, are left out.
- `src/runtime_hooks.c` (the hook engine for respec, camera, IK, papers and save slots) has a Windows stub:
  COFF symbol, the argument in `rcx`, 32 bytes of shadow space.

The only warnings in the runtime are 3 old unused-variable ones in upstream code. Every DLL the executables
import is in MSYS2 CLANG64 or Windows. `bb-probe.exe` is a GUI-subsystem program (SDL3's `-mwindows`, as in
Supermedo's), so `run.py` passes it the standard handles for its log.

## Open
- Not yet run on Windows: the game itself, online, the launcher and the package. Those need the PC.
- The C tests (`build.sh --test`) have no Windows path.
- Cross-play is not tested: Windows↔Mac and Windows↔Linux, with the Windows PC hosting and joining.
