# Native Metal renderer for macOS: plan (2026-10-17)

> **Status 2026-10-19: not started, low priority.** Huax-1839/bloodborne-macos built a native Metal backend and
> measured only ~1.1x over MoltenVK in the clinic (M2). The Mac's 44 FPS ceiling turned out to be GPU load from the
> game's effects: with them off, native 720p holds ~56-60 FPS on the M4 Air through MoltenVK. The nearer wins are
> MoltenVK patches (upstream PRs #2867, #2837) and fixing the tessellation-hoist flicker.

Goal: the game smooth on Apple Silicon (M4, 16 GB) by rendering straight to Metal instead of
Vulkan through MoltenVK. The CPU side stays as it is: the x86-64 runtime under Rosetta 2
(`docs/bbcoop/brief.md`, macOS decision). An arm64 CPU build is a separate, later question (Rosetta's
lifetime), and this renderer carries over to it unchanged.

## Why the GPU path, not a CPU recompile

Measured on the `mac` branch, 2026-10-12: clinic 11–17 FPS, Cathedral Ward 14, title 45–49,
GPU-bound. Every frame is ~700 Metal encoders (~210 render passes, ~285 compute) on a tile-based
GPU. Each mid-pass compute dispatch or GPU label ends the render pass and flushes its tiles to
memory. With minimum settings, labels off and mid-pass dispatches skipped (diagnostic, wrong
image), Cathedral Ward reached 40 FPS. That is the headroom: pass structure, not CPU and not
shader quality. Recompiling the CPU code would emit the same command stream into the same
translation and leave FPS as it is.

## What upstream changed (0.6-pre1, tag of 2026-10-15, branch `testing`)

Upstream deleted `gpu/shadps4` and replaced it with its own code (`docs/SHADPS4_EXIT_PLAN.ru.md`
on that tag). The SPIR-V it emits for the game's shaders is byte-identical to the old SPIR-V. The new tree splits as follows:

| Module | Lines | Vulkan in it | For Metal |
|---|---|---|---|
| `gpu/cp` (PM4 decoder, command processor) | 4.7k | 1 reference | reuse |
| `gpu/gcn` (GCN definitions) | 4.7k | none | reuse |
| `gpu/shader/frontend`, `ir` | 16.5k | none | reuse |
| `gpu/shader/backend` (IR to SPIR-V via sirit) | 6.0k | none | reuse, then SPIR-V to MSL |
| `gpu/memory`, `gpu/layer` | 3.9k | 23 refs in layer | reuse; the layer's Vulkan import becomes a Metal one |
| `gpu/render` (rasterizer, pipelines, scheduler, presenter) | 18.3k | 46 of 56 files | **rewrite for Metal** |
| `gpu/textures`, `gpu/buffers` | 9.2k | 24 of 33 files | **rewrite the GPU half** |
| `gpu/vulkan` (instance, swapchain, pools) | 2.6k | all | replaced by a small `gpu/metal` |
| `gpu/upscale` (FSR) | 7.4k | 16 of 19 files | replaced by MetalFX |

It also brings BBSC (seamless co-op, up to five hunters) in its online module, the copy shader
done as batched copies (`render/shader_hle.cpp`: ~57 dispatches a frame with ~1024 ranges each
become one multi-copy dispatch per group) and fewer submits. A Metal renderer built on the old
shadPS4 `renderer_vulkan` would be written against code upstream no longer has, so this one
builds on 0.6.

## What others did (prior art)

- **Cemu** (`src/Cafe/HW/Latte/Renderer/Metal`, MPL-2.0): a full native Metal backend for a
  console GPU.
  - It keeps one live encoder and switches render, compute and blit encoders on demand.
  - A new render pass begins only when the bound attachments change (`GetRenderCommandEncoder`).
  - Clears use their own pass with `LoadActionClear`.
  - It has a rasterization-off vertex pipeline (`MetalVoidVertexPipeline`) meant to do buffer copies inside a render
    pass on Apple GPUs. That path is commented out as "TODO: fix performance issues", so the
    in-pass vertex-shader trick is not free.
- **Wind Waker HD recomp** (`~/Documents/code/zelda/wwhd-recomp/runtime/src/gfx`): a native
  Metal renderer of ~3k lines plus Cemu's Latte decompiler emitting MSL. It has:
  - asynchronous MSL compiles with a 25 ms wait before a draw is skipped,
  - a cache of pipeline recipes with a head start at launch,
  - de-duplicated recipe records.
  
  The ISA differs (R600 vs GCN), so the decompiler can't be reused, but these patterns can (`docs/performance.md` there).
- **Dolphin**: its native Metal backend beat MoltenVK by far more than its developers expected
  in GPU-heavy scenes (progress report July/August 2022).
- **xenia-mac**: a Metal port of Xenia that converts shaders with Apple's Metal Shader
  Converter. That tool takes DXIL, not SPIR-V, so it doesn't fit our translator.
- **Apple**:
  - Load and store actions run per tile, so a clear is cheaper than a load and a store
    can be skipped.
  - Serial compute makes every write visible to every later dispatch.
  - Metal 4 (macOS 26+, so on this Mac) adds a unified compute encoder that does blits too,
    intra-pass stage barriers, render passes that suspend and resume across command buffers,
    placement sparse resources and residency sets.

## Shaders

The game's GCN goes through upstream's translator to SPIR-V, then SPIRV-Cross to MSL, compiled
by Metal. This is what MoltenVK does today, so the game's shaders are already known to compile.
The known failure (an FS reading inputs the VS does not export, `fs_fbe7c4b6`) is fixed at
linking time, where both stages are known. An own IR-to-MSL emitter is an option later, only
if SPIRV-Cross output costs measurable GPU time. Compiled pipelines go into an
`MTLBinaryArchive`, and the recipe cache follows Wind Waker HD's.

## The pass planner (where the FPS is)

The rasterizer records a frame's work as a list (draws, dispatches, copies, clears, labels).
At each submit point a planner turns that list into Metal encoders:

1. **Hoist** compute dispatches, copies and clears out of a render pass when they don't read
   anything the pass writes earlier, and nothing earlier in the pass reads what they write.
   Upstream's copy HLE already reads each copy's (destination, source, count) list on the CPU,
   so those hazards can be checked exactly. Hoisted work runs before the pass in one compute
   encoder.
2. **Merge** consecutive passes with the same attachments when nothing between them forces a
   split.
3. **Load/store actions:** `DontCare` where the next use overwrites the attachment or never
   reads it (depth after the frame's last depth pass), `Clear` instead of a clear draw,
   memoryless for transient attachments.
4. **Labels** (`WriteLabelOnGpu`) are moved to the pass end when nothing in the pass waits on
   them.
5. **What can't move ends the pass**, as today. The planner counts every split by reason, so
   the next fix is always the largest count.

## Memory

Apple Silicon has unified memory. Guest memory is wrapped as Metal buffers
(`newBufferWithBytesNoCopy` on page-aligned ranges), so buffers need no upload copies. Write
traps and the layer's bookkeeping stay as they are. Textures still detile into their own
`MTLTexture`s.

## Milestones

| # | What | Done when |
|---|---|---|
| 0 | Merge 0.6-pre1 into `main`, re-apply the Rosetta port on top, measure 0.6 through MoltenVK (FPS and encoders per frame at the title, clinic and Cathedral Ward) | The game runs on the Mac from 0.6, and the baseline numbers are written down |
| 1 | `gpu/metal`: device, CAMetalLayer presenter, SPIR-V to MSL pipelines, buffers and textures, one encoder per pass with no planner yet | The title screen renders through Metal alone (`BB_RENDERER=metal`), matching a MoltenVK frame dump |
| 2 | The whole rasterizer on Metal: every draw, dispatch, copy, query and label the game uses | The clinic and Cathedral Ward render correctly; the frame-dump diff against MoltenVK is clean |
| 3 | Pass planner (hoist, merge, load/store), with split counts by reason in the overlay | Cathedral Ward FPS at least doubles over milestone 0's baseline |
| 4 | MetalFX upscaling in place of FSR; pipelines in a binary archive; frame pacing on CAMetalDisplayLink | A smooth first session with no compile stutter |

Milestone 1 is where the approach is checked. If one frame through Metal does not show far
fewer encoders and a shorter GPU time than MoltenVK for the same frame, the plan is re-weighed
before milestone 2.

## Rules carried over

- Linux keeps Vulkan; Metal code is `#ifdef __APPLE__` or in files only the Mac build compiles.
  Upstream files get small, marked edits only.
- Fixframe's Metal SDK (`~/Documents/code/fixframe`) can report the new renderer's draws for
  debugging.
- No game files, dumps or captured frames in the repo.
