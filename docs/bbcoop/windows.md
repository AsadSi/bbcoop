# bbcoop on Windows

Since 2026-10-21 (Asad: cross-play tests on Linux, macOS and Windows). A native 64-bit build, built
in MSYS2 CLANG64 on Windows. It plays online with Linux and macOS bbcoop: everyone is on the shadPS4 0.20
module (gen 20).

## Play on the Windows PC, step by step
For Asad's PC (no Claude there). The zip is made on the Mac (below), so the PC builds nothing and
nothing has to be pushed. The Mac app and the zip must come from the same commit, or Join refuses.

**On the Mac**
1. `bash packaging/windows/package.sh` makes `dist/bbcoop-windows.zip`; it prints the build, e.g.
   `Build 8c1435db25bf`. Rebuild the Mac app from the same commit (`bash packaging/macos/package.sh`).
2. Send the zip to the PC over Tailscale: `tailscale file cp dist/bbcoop-windows.zip <pc-name>:`
   (`tailscale status` lists the PC's name). A USB stick or a cloud drive works too.

**On the PC, once**
1. Install Tailscale (tailscale.com/download), sign in to the same tailnet as the Mac. A file sent
   from the Mac shows up in Tailscale's icon by the clock (or in Downloads).
2. Right-click the zip, Extract All…, to `C:\` (you get `C:\bbcoop-windows`). Not under Program Files.
3. Double-click `C:\bbcoop-windows\Bloodborne.exe`. SmartScreen ("Windows protected your PC"): More
   info, Run anyway. It asks for the Bloodborne folder (with `eboot.bin`) and a hunter name.
4. Press PLAY once offline. The first start builds the respec mod and the shader cache: a few minutes.
   Close the game window once you reach the title screen or your character.

**Play together**
1. Mac and PC: Tailscale on. In the launcher, Online co-op: one picks **Host a game**, the other
   **Join a game** and picks the host from the list (or types its 100.x.y.z address). Then PLAY.
2. Windows Firewall asks once each for `python`, `shadnet` (when hosting) and `bb-probe`: tick
   **Private and Public** and click Allow. If you clicked away the question, Windows Security, Firewall &
   network protection, Allow an app through firewall, and tick both boxes for those.
3. Load your character. The bells ring by themselves; the guest arrives in the host's world. Try both
   ways: the PC hosting, then the Mac hosting.

**Logs to send back** (all inside `C:\bbcoop-windows`):
- `user\last_run.log`: the game's output for the last start.
- `user\log\bbsc-timeline-*.txt`: the newest one, co-op events.
- `host\logs\shadnet.log`, `host\logs\gameserver-console.log` and the rest of `host\logs`: when the
  PC hosted.
- `out\respec_mod.log`: when the launcher said the respec mod was not built.
- For a crash or a black screen: the launcher's Log page, copied in full.

For the browser AI on the PC: say it is "a Windows port of a PS4 game with its own launcher; I unzipped
it to C:\bbcoop-windows", paste the error and the end of `user\last_run.log`. Don't let it make you
install Visual C++, DirectX or Python: the package has what it needs.

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

## One command
In PowerShell, with Git for Windows (`winget install Git.Git`); the repo is public:

```powershell
git -c core.autocrlf=false -c core.longpaths=true clone -b main https://github.com/AsadSi/bbcoop.git C:\bbcoop; cd C:\bbcoop
powershell -ExecutionPolicy Bypass -File install.ps1   # asks for your Bloodborne folder; -GameDir D:\Games\CUSA00900 skips the picker
```

`install.ps1` installs MSYS2 and Python (winget), the packages below, builds and packages, unpacks to
`%LOCALAPPDATA%\bbcoop`, sets the launcher's game folder and puts a Bloodborne shortcut on the desktop. The steps below
are the same thing by hand. Not yet run on Windows.

## Build (on the Windows PC)
Install [MSYS2](https://www.msys2.org) (`winget install MSYS2.MSYS2`) and a python.org Python 3.13+ for
the package. Clone it from PowerShell with Git for Windows (`winget install Git.Git`):

```powershell
git config --global core.longpaths true
git config --global core.autocrlf false   # LF endings: bash scripts and the FSR-Vulkan patches need them
git clone --recursive -b main https://github.com/AsadSi/bbcoop.git C:\bbcoop   # --recursive pulls a large server submodule too; install.ps1 needs only 3
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
cd /c/bbcoop   # cloned from PowerShell above
bash build.sh                       # out/bb-probe.exe, bb-gpu-capabilities.exe, bb-play.exe
bash packaging/windows/package.sh   # dist/bbcoop-windows.zip (after the game runs)
```

To play from source, use the Windows Python in PowerShell. `run.py` adds `C:\msys64\clang64\bin` to `PATH`
for the DLLs:

```powershell
cd C:\bbcoop
py -3.13 -m pip install soulstruct==2.6.0      # the respec mod
py -3.13 launcher\bbport_launcher_win.py       # or: $env:BB_GAME_DIR="D:/Games/CUSA00900"; py -3.13 run.py
```

The package freezes the launcher with PyInstaller into `Bloodborne.exe`, with soulstruct for the respec
mod. Players need no Python. `run.py` takes the same variables as `run.sh`. From source, the respec mod
needs soulstruct in the Python that runs `run.py`, or in `BB_RE_PYTHON`.

## The package from the Mac
`bash packaging/windows/package.sh` on macOS or Linux runs `package-cross.sh`:
- the executables from `scripts/windows/cross-build.sh`, including `shadnet.exe` (server/, cross-built
  with MSYS2's Qt 6.12 and Homebrew's Qt tools and the Mac server build's protoc) in `bin/shadnet/`;
- every DLL they load, followed through the DLLs' imports in the MSYS2 sysroot, with MSYS2's own C++
  runtime (libc++, libunwind, winpthread, libomp), not llvm-mingw's; it stops if one is neither there nor
  Windows' own, and `scripts/windows/check_imports.py` stops it if any imported function is missing from
  the shipped DLL. The first PC run (2026-10-26) exited 0xC0000139 ("entry point not found"): MSYS2's
  DLLs import `_Unwind_Resume` from libc++.dll, which llvm-mingw's libc++.dll lacks. Wine never saw it
  (it stubs missing imports);
- PyInstaller cannot freeze for Windows from a Mac, so the package carries python.org's Windows Python
  3.13 (its MSIs unpacked with `msiextract`, `brew install msitools`) with soulstruct 2.6.0 and the
  Mac respec Python's versions of its wheels; `Bloodborne.exe` is `packaging/windows/launcher.c`,
  which runs `python\python.exe -s -E launcher\bbport_launcher_win.py` without a console.
  Mrsuss60/bloodborne_pc_windows_port ships a Python the same way;
- `BUILD` with the commit, as the Mac app.
The MSYS2 path (`package.sh` in MSYS2, `install.ps1`) still freezes with PyInstaller.

Checked under Wine 10 (Debian's, in an x86-64 Docker container; Homebrew's Wine is disabled):
`shadnet.exe` starts and creates an account through `bbcoop_online.make_account`; `bb-probe.exe` and
`bb-gpu-capabilities.exe` load every DLL; the launcher window opens from `Bloodborne.exe`; tkinter,
soulstruct and the launcher's modules import; `run.py --cpu-only` with the game runs its code (16
threads, audio, every bbcoop hook, the red-zone sites) up to the first GPU call, as CPU-only does. numpy stops in Wine's own `ucrtbase` (no `crealf`), so the
respec tool is not checked there; real Windows has it.

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

## Fixed before the first run (2026-10-25)
Found by reading the code paths that differ on Windows; all marked `bbcoop`, the Mac and Linux unchanged:
- `src/runtime_context_lock.c`: its asm calls `enter`/`leave` System V style; on Windows they took the
  Microsoft ABI, so the render thread's frame job crashed. Now `sysv_abi`.
- `src/runtime_papers.c`: the game's param lookups were called with the Microsoft ABI.
- `gpu/bbnet20/bbnet_glue.cpp`: BBSC's code pages are made writable with VirtualQuery/VirtualProtect
  (only `/proc/self/maps` before: BBSC's first code write faulted).
- `gpu/shim/bbport_settings.cpp`, `bbport_overlay.cpp`: `std::rename` does not replace a file on
  Windows: in-game menu changes were never saved.
- `src/runtime_respec.c`: old respec backup folders are removed (Windows' `remove()` keeps folders).
- `scripts/patches.py`: Intel CPUs are found through `PROCESSOR_IDENTIFIER` (no `/proc/cpuinfo`), so
  they get the black tone-map fix.
- From Supermedo/bloodborne_pc: guest memory committed as the game maps it when the pool exceeds the
  commit limit (7c9ebcb, in `gpu/platform/win32/posix_memory.cpp`), Windows certificate trust for HTTPS
  (a946924, `gpu/bbnet20/include/windows_trust.h`), FMOD threads above normal priority (08df6af).
- The Hunter's Dream's UDP sockets ignore ICMP port unreachable, as the vendored `host_net.cpp`.
- Host/Join: Tailscale in Program Files, no console windows, the servers end with the launcher (a job
  object), `shadnet.exe`.
Not taken: Mrsuss60's 7c0e5f0 crash workarounds (another tree layout, not Windows-specific).
Left as is: BBSC looks for a save under `%APPDATA%\shadPS4` before the port's save folder exists; only
saves written during the run count, so it never picks one.

## First PC run (2026-10-26, RX 5700 XT, Windows)
- 2026.10.26: exit 0xC0000139 before the game started (llvm-mingw's libc++.dll; fixed in b767b0d).
- 2026.10.26b: the game ran (menus, save loaded, audio over WASAPI) and then stopped on the game's own
  panic "Mutex is not initialized" (DLLightMutex.cpp 126) as the title screen's idle movie
  (`sprj_advertise.mp4`) started. Traced in the game's code: the DL thread object of the movie's audio
  thread (`Mv_AudioPlayThread`, made at +0x28fe219) was destroyed (its mutex destructor +0x207ed40
  clears the flag) before the new thread's entry (+0x263b1e0) locked it: the game tore the movie down
  at once. Ruled out: FFmpeg opening that file on Windows (works under Wine), the winpthread mutexes,
  the game's thread entry ABI. The movie player gave up silently there; f486342 logs why, and
  `BB_GPU_LOG=info` shows every event it sends the game. Workaround meanwhile: don't leave the title
  screen idle (press a button within half a minute); a new character's opening movie may hit it too.
- Also in that log: AMD's driver refused storage usage on BC1/BC7 textures (fixed in 747b08d; gone in
  the 2026.10.26c run).
- 2026.10.26c (same panic, a character loaded, no AvPlayer error logged): the movie class (vtable
  0x52e3800: +0x10 init loads sysmodule 0xa5, +0x20 open at +0x28fddc0 makes the audio thread, +0x30
  close at +0x28fe2e0) closes right after opening. Close stops the audio object (+0x29002d0), which
  waits only when the audio thread has marked itself running (state 1), so a thread not yet started is
  not waited for; the DL thread's destructor (+0x208b440) then detaches it (scePthreadDetach, not a
  join) and destroys its mutex; the thread starts and panics. On the Mac the thread opens its audio
  port right after creation instead. Not the cause: the AvPlayer failing (nothing logged), its
  callbacks' ABI, red-zone sites (none near), thread attributes, timed waits' clocks. Next: a run with
  `BB_GPU_LOG=info` (the player's events and "the game closes the player"), and whether the movie
  showed at all before the crash.

## Open
- Not yet run on Windows: the game itself, online and the package. Those need the PC.
- The C tests (`build.sh --test`) have no Windows path.
- BBSC's memory scan on Windows skips pages the port keeps without access (Linux reads them through
  `/proc/self/mem`): the first co-op timeline from the PC must show the kit given and the param tables
  found.
- Cross-play is not tested: Windows↔Mac and Windows↔Linux, with the Windows PC hosting and joining.
