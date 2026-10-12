# macOS (Apple Silicon)

**Status:** the game runs on an M1 Pro (16 GB, macOS 26, MoltenVK 1.4.2): the title screen, the
save and the Hunter's Dream at the game's 30 FPS with the Mac defaults (see
[Performance](#performance) and [Known limits](#known-limits)). The 60 FPS mode is not measured
yet.

## Why an x86-64 build under Rosetta 2

The port runs the game's own x86-64 code inside the loader's process. On Apple Silicon only
Rosetta 2 can execute it, and Rosetta translates whole processes: the loader, the runtime, the GPU
library and every dependency are built for x86-64. Rendering still runs on the Apple GPU: Vulkan
goes through MoltenVK to Metal. Rosetta executes AVX/AVX2; `run.sh` sets
`ROSETTA_ADVERTISE_AVX=1` so CPUID reports them too.

## Setup

1. Xcode Command Line Tools (`xcode-select --install`) and, from the (arm64) Homebrew:
   `brew install cmake ninja pkgconf glslang nasm`.
2. x86-64 libraries from [vcpkg](https://github.com/microsoft/vcpkg) (Homebrew no longer installs
   as x86-64 on Apple Silicon):

   ```bash
   git clone https://github.com/microsoft/vcpkg.git && ./vcpkg/bootstrap-vcpkg.sh -disableMetrics
   ./vcpkg/vcpkg install --triplet x64-osx --host-triplet arm64-osx \
       sdl3 vulkan-loader vulkan-headers fmt magic-enum robin-map vulkan-memory-allocator \
       xxhash miniz zydis xbyak "ffmpeg[avcodec,avformat,swscale,swresample]" \
       boost-headers boost-cmake boost-asio boost-container boost-container-hash boost-icl \
       boost-intrusive boost-pool boost-preprocessor
   ```

   `build.sh` takes the prefix from `BB_MACOS_DEPS`, else `$VCPKG_ROOT/installed/x64-osx`. The
   online module (`gpu/bbnet`) is left out without its libraries; the game plays offline.
3. MoltenVK: a universal build from the
   [Khronos releases](https://github.com/KhronosGroup/MoltenVK/releases) (`MoltenVK-macos.tar`,
   `MoltenVK/dynamic/dylib/macOS/`). `run.sh` points the Vulkan loader at its manifest with
   `BB_MOLTENVK_ICD=/path/to/MoltenVK_icd.json` (`VK_DRIVER_FILES` works too).
4. The game: your decrypted `CUSA03173` dump, version 1.09 (see the README).

## Running

From a terminal (the first start builds `out/bb-probe`, about a minute):

```bash
BB_GAME_DIR=/path/to/CUSA03173 VCPKG_ROOT=/path/to/vcpkg \
BB_MOLTENVK_ICD=/path/to/MoltenVK_icd.json bash run.sh
```

`BB_FPS=60` selects the 60 FPS patch (`30` is the game's own rate), `BB_FRAME_STATS=1` prints
frame statistics every 5 s. macOS lets no app take the keyboard focus by itself, so `run.sh`
brings the game's window to the front through System Events once it is up (macOS may ask to allow
the terminal to control System Events); clicking into the window works as well.

**Launcher.** The launcher is GTK 4 / libadwaita. With them from Homebrew
(`brew install gtk4 libadwaita pygobject3 adwaita-icon-theme`) it runs with Homebrew's Python,
which is the one that has PyGObject:

```bash
/opt/homebrew/bin/python3.14 launcher/bbport_launcher.py
```

Without GTK the window is unavailable, the rest is not: `--set key=value ...` stores launcher
settings and `--play` starts the game with them. The Mac settings are `macos_deps` (the x86-64
prefix) and `moltenvk_icd`:

```bash
python3 launcher/bbport_launcher.py --set game_dir=/path/to/CUSA03173 fps_mode=60 \
    macos_deps=/path/to/vcpkg/installed/x64-osx moltenvk_icd=/path/to/MoltenVK_icd.json
python3 launcher/bbport_launcher.py --play
```

**Defaults on macOS.** The upscaler (FSR 3.1) and its object motion vectors are off and the FPS
counter is on: with them the M1 Pro held 17-20 FPS in the Hunter's Dream, 30 without. The
in-game menu and `bbport.ini` (`upscaler`, `object_motion`, `show_fps`) change them.

**Keyboard.** I/J/K/L are the d-pad (menus), WASD moves, the arrows and the mouse turn the
camera, Space is ✕, left Shift ◯, E ▢, Q △, Enter Options, Tab the touchpad; the launcher's
*Controls* remaps them. **F10** (fn+F10) opens the in-game menu (Mac keyboards have no Insert
key; L3+R3, Z+C on the keyboard, works too).

## How the port differs from Linux

| Linux | macOS |
|---|---|
| Guest pool: a `memfd`, mapped at any offset | A named Mach memory entry (`mach_make_memory_entry_64`), mapped with `mach_vm_map` (`pool_map`) |
| Released direct memory: `fallocate` punches a hole | Pages holding data (per `mincore`) are zeroed, then released with `MADV_FREE_REUSABLE` (`pool_punch`) |
| Direct memory from `0x1000000000` | Apple Silicon reserves `0x1000000000`-`0x7000000000` for the GPU, also in Rosetta processes: direct memory starts at `0x7800000000`. The low range (image at `0x800000000`, stacks) keeps Linux's addresses below the reserved one |
| Host objects the guest sees (thread records, TLS, mutexes, rwlocks) come from `malloc`, i.e. the non-PIE brk heap below 1 TiB | macOS's `malloc` is far above 1 TiB and the game packs pointers into 40 bits (its locks keep `scePthreadSelf` owners): they come from low mappings (`runtime_low_malloc`, `GUEST_*` in `runtime.h`) |
| `mov rax, fs:[0]` rewritten to `gs:[0]`, GS base = guest TCB (`arch_prctl`) | GS is the pthread TSD array: the loader points those loads at `gs:[slot*8]`, a TSD slot holding the guest TCB (`patch_tls_loads`, `darwin_compat.c`) |
| Signal contexts: `uc_mcontext.gregs[REG_*]` | `uc_mcontext->__ss`, exposed as the same array (`BB_GREGS`, `gpu/shim/bbport_ucontext.h`); write faults on protected pages arrive as `SIGBUS` |
| The game on the main thread, the window on a thread of its own | Cocoa runs windows on the main thread only: the window keeps it (`bbgpu_run_window_loop`, a Metal view) and the game runs on a thread of its own (`run_guest`) |

Smaller Darwin stand-ins (clocks, timed locks, per-thread CPU times, `gettid`,
`process_vm_readv`, `MAP_FIXED_NOREPLACE`) are in `src/darwin_compat.h` and, for the GPU library,
`gpu/shim/bbport_darwin.h` (included into every source of `libbbgpu` on Apple).

On the GPU side:

- **Portability:** the instance enables `VK_KHR_portability_enumeration`, the device
  `VK_KHR_portability_subset`. `robustBufferAccess2`/`robustImageAccess2` are enabled only where
  the driver has them (Metal bounds accesses itself).
- **No `nullDescriptor` on MoltenVK.** `NullResources` (`vk_null_resources.h`) stand in: a zeroed
  buffer and zeroed 1×1 images with a view per image type (D32 for depth slots, 4-sample images
  for multisampled ones). `null-resources-test` samples every stand-in through the type a shader
  declares.
- **No sparse residency on MoltenVK.** Elsewhere the buffer cache keeps guest memory in sparse
  4 GiB arenas. Here they are dense buffers of `BB_ARENA_MB` (256 MiB; Metal commits a buffer
  whole on its first GPU use). Their blocks are resident with the arena: `EnsureResident` records
  them and points the BDA page table there. A binding that spans several arenas merges them (the
  contents copied, the old buffers released once the GPU is past them; `arena-span-test`); the
  log shows `GPU: merged … buffer arenas`. The PC memory model (in-place and VRAM blocks) stays
  off, as on other non-AMD GPUs.
- **No push descriptors.** Once a set has 11 or more bindings, MoltenVK 1.4 gives the push
  constants a Metal buffer index already in use and the shader library fails to compile.
- **No `NoContraction`.** SPIRV-Cross implements it with an `[[clang::optnone]]` helper for every
  add, subtract and multiply, so Metal compiled every game shader unoptimized (GPU 193 → 38 ms a
  frame at 720p). On Apple `Profile::skip_no_contraction` leaves it out and the vertex position is
  `Invariant`, so passes drawing the same meshes compute it identically (without it lighting
  flickers). The shader cache version is `0x100 +` the Linux one.
- **Draws with no effect are skipped** (no attachments, no pixel shader, no stores), unless
  Vulkan occlusion queries count them (`BB_OCCLUSION_QUERIES=1`): the game issues some over a
  16384×16384 area, which a tile-based GPU walks whole.
- **Geometry shaders and multisampled storage images:** Metal has neither. Those draws and
  dispatches are skipped, logged once per program and counted at exit.

## Build and test

```bash
VCPKG_ROOT=/path/to/vcpkg bash build.sh --test
VK_DRIVER_FILES=/path/to/MoltenVK_icd.json out/bb-probe --vulkan-only
ninja -C out/gpu motion-history-test motion-shader-test ui-composition-test cp-decoder-test \
    settings-test scene-resolution-test taa-shader-test camera-motion-test \
    upscaler-support-test null-resources-test arena-span-test window-test
```

Run each test from `out/gpu` with `VK_DRIVER_FILES` set; `window-test` opens a window on the main
thread for 3 s. Verified on the M1 Pro: the runtime tests (including the guest thread pointer
through the TSD slot on two threads), `--vulkan-only` and all twelve GPU tests pass.
`translation-memory-test` does not build on any system at this version (it uses a
`BufferCopyShaderHash` the code no longer has).

## Performance

M1 Pro, 1080p output, Hunter's Dream (~700 draws per frame):

| Setting | FPS | Notes |
|---|---|---|
| FSR 3.1 + object motion (Linux defaults) | 17-20 | GPU 98-99% busy |
| Mac defaults (no upscaler, no object motion) | 30 (the game's rate) | GPU thread idle ~65% |

`BB_FRAME_STATS=1` prints FPS, draws, GPU waits and write faults every 5 s;
`MVK_CONFIG_SHADER_DUMP_DIR=<dir>` dumps the MSL MoltenVK generates for each shader.

## Known limits

- **60 FPS** (`BB_FPS=60`) is not measured yet; at 30 FPS the GPU is already busy in the Hunter's
  Dream, so heavier areas will not hold 60.
- **Geometry shaders / multisampled storage images:** skipped (see above); what they draw is
  missing.
- **FSR 4, DLSS** and other vendor-specific features are unavailable on Apple GPUs; FSR 3.1 and
  TAA work but cost the frame rate above.
- **Online play** needs the online module's libraries (protobuf, OpenSSL, miniupnpc, …), not
  covered by the vcpkg list above.
- The **PC memory model** (`BB_PC_MODEL=1`, the layer's memory module) is not ported: it needs the
  game's memory imported as host memory (`VK_EXT_external_memory_host`, which MoltenVK has) and
  is off as on other non-AMD GPUs.
