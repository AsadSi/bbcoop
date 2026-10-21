# Debug tools for the port's open issues

Written 2026-10-14. These outside tools fit the issues this repo has hit, on the Linux setups it targets (the
Steam Deck and the RX 7800 XT, both RADV). Each entry builds on what `tools/` already has (frame dumps,
`BB_TOGGLE_FILE` A/B, `BB_PAD_FILE` scripted input, `load_watch.sh`). Versions and licences are from research
for Fixframe (`~/Documents/code/fixframe/docs/brief.md`, "Bugs across frames"), checked 2026-10-14. The port
already links GPL code (shadPS4), so every licence below is compatible.

## Models dipping into the ground at an animation change (`docs/animation_re.md`)

The dip lasts a few frames, so a single-frame capture (`BB_CAPTURE_TRIGGER`) misses it. Put the cause and the
picture on one timeline:

- **Tracy** (v0.14.1, 2026-08-22, BSD-3-Clause; https://github.com/wolfpld/tracy) has a C API that fits the
  `runtime_*.c` code.
  - From the `runtime_ik.c` hook, call `TracyCPlot("ik err", err)` per character with `+0x260`, and plot `+0x1dc`
    too.
  - Call `TracyCFrameMark` per flip, and `TracyCFrameImage` with a small (e.g. 320x180) readback of the present
    image. Its `offset` argument lets the readback lag a frame, so the game never waits.
  - In the profiler, hover a frame for its image, step frames in the playback window, and read the plotted
    error underneath. A dip that lines up with an `err` spike settles candidate 1 against 2 directly, with no
    A/B guessing.
  - Plot the animation id as well, as a step plot: the change shows as an edge.
- **Flag it automatically.** A dip is an excursion that comes back within a few frames. Flag a value further
  than k·1.4826·MAD from its rolling median, using about 0.5 s of game time as the window, not a frame count.
  Start k at 4, with an absolute floor. Don't smooth before flagging: smoothing hides one-frame spikes.
- **Capture the frames around it.** RenderDoc (v1.46, 2026-08-31, MIT) works on Linux with RADV. Its in-app API
  (`renderdoc_app.h`: `RENDERDOC_GetAPI`, then `TriggerMultiFrameCapture(n)`) captures the next n frames, so
  calling it from the IK hook when `|err|` passes a threshold catches the dip's frames as full captures. It only
  captures forward, which is fine here, because the error comes before the visible dip.

## Cape clipping and other clipping

- **RenderDoc**, on a frame captured as above:
  - the *Depth Test* overlay on the cape draw shows its failed fragments in red, which is the part hidden
    inside the body or the ground;
  - *Pixel History* on a clipping pixel lists every draw that touched it.
- **A clipping number per frame,** with the game's own data: plot the distance from the cape bones to the body
  bones through Tracy. The port runs the game's own code, so the bones are there to read. A GPU-side meter for
  this is planned in Fixframe (T2, see below).

## Upscaler ghosting and smears (FSR 4 Ultra Performance on the character, messenger history)

- **NVIDIA FLIP** (BSD-3-Clause; https://github.com/NVlabs/flip) gives a perceptual difference map between
  two images. Run it on two `tools/frame.sh` dumps of the same scripted spot (`BB_PAD_FILE`), for example
  `BB_UPSCALER=off` against FSR 4. It gives a score and a heatmap instead of eyeballing, which helps
  `play_ab.sh`-style comparisons.
- Tracy's frame images (above) show a smear across frames next to the plotted jitter phase or reset flags.

## Loading stutters and the GPU command thread (`docs/parallel_gpu.md`)

- **Tracy zones.** `TracyCZoneN` around the stages (PM4 decode, pipeline select, `bb:DrawRec`, guest memory
  copies, fence waits) shows per frame which thread holds a 40–50 ms load freeze, which `load_watch.sh`'s
  5-second averages can't. Name the threads (`TracyCSetThreadName`).
- **Radeon GPU Profiler** (AMD, free, proprietary) through RADV: start the game with `MESA_VK_TRACE=rgp` and
  `MESA_VK_TRACE_TRIGGER=<file>`, then touch the file to save a trace of the next frame to `/tmp`. It shows GPU-side
  time per pass and per barrier: where the ~1.4 ms of FSR 4 goes on the Deck.
- `perf record -g` on the busiest thread `load_watch.sh` names, for a CPU-side hotspot under a stutter.

## Crashes, hangs, corruption

- **Vulkan validation layers** with synchronisation validation (`VK_LAYER_KHRONOS_validation`, from the
  LunarG SDK or the distro's package; Apache-2.0). They catch missing barriers and use-after-free of
  Vulkan objects: the kind of ordering bug behind the EOP-label heap corruption.
  - Turn on sync validation with `VK_VALIDATION_VALIDATE_SYNC=1`.
  - Expect a large slowdown, so use it on a short `soak.sh` run.
- **`RADV_DEBUG=hang`** writes a report under `~/radv_dumps_*` (the last command buffers, shader and state)
  when the GPU hangs.

## Regressions (a fix that later breaks)

Copy Dolphin's FifoCI (BSD-2-Clause; https://github.com/dolphin-emu/fifoci):
1. A fixed route: a snapshot (`BB_SNAPSHOT`) plus `BB_PAD_FILE` input.
2. The same frame numbers dumped with `tools/frame.sh` on every run, stored by hash.
3. A comparison against the last accepted run.

Run the route twice first. If the frames aren't identical, compare draw counts and a few plotted values, and
compare pixels only with a tolerance (apitrace 14.0's `snapdiff.py` default fuzz is 0.05, MIT) and masks for
particles. PCSX2's `pcsx2-gsrunner` does the same and also diffs draw counters (GPL-3.0, so ideas only).

**GFXReconstruct** (vulkan-sdk-1.4.363.0, 2026-09-28, MIT) can also hold a bad frame for later. Set
`GFXRECON_CAPTURE_TRIGGER=F3` and press F3 to capture a frame range, then use
`gfxrecon-replay --screenshots <frames>` to compare a driver or build change against the same commands. The
guest-memory traffic makes its files large.

## Fixframe

Fixframe (`~/Documents/code/fixframe`, private for now) is Asad's live render debugger. It is a Vulkan layer
(`VK_LAYER_FIXFRAME_debug`) that finds the draw behind a pixel, scrubs the frame, and compares draws across
captures.
- It has been run with this port only on macOS (MoltenVK).
- The Linux build is its milestone M8.
- Its rolling 10-second buffer, timeline and `fixframe_plot(name, value)` (T1), and the clipping meter (T2),
  are aimed at exactly the dip and the cape. They are not built yet.
