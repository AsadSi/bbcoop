# Models dipping into the ground at an animation change: research notes (v1.09)

Image offsets, as in `docs/respec_re.md`. This is static analysis and web research from 2026-10-11; the game was not run.

**Symptom** (Asad, "always there"): when a model starts a new animation, it briefly moves down into the ground, then
returns. Seen on the Plain Doll (e.g. opening Level Up) and on the messengers. This is what "messengers popping up and
disappearing" meant, so the 5 m emerge rule (`docs/messengers_re.md`) is probably not the cause.

## Candidate 1: foot-IK height correction (per-frame gains, vanilla)
- **Where:** ChrCtrl::Update `0x1514da0(ctrl, dt = xmm0)`, called each frame from the ChrIns update at `0x18be236`. The
  correction is `0x1515087`–`0x15151f1`.
- **Gating:** the behavior module (`[chr+8]->0x3b0->0x30`) byte `+0x384`, and ChrCtrl `+0x28c`, which is set to 1 at
  `0x1511f8b` ("IKエラーによる高さ補正を行うか").
- **Error:** it takes the minimum foot-IK vertical error over the legs that hit something (leg array
  `[[beh+0x10]+0x18]+0x58`, 0x30 bytes each, float `+0x28`, hit byte `+0x2c`, mask `[beh+0x10]+0x7c`). The physics
  module's `+0x2e4` lowers it further when negative.
- **Step, once per frame with no dt:**
  - With a leg hit, `err(+0x260) += ON × min`. ON = 0.1, a .data global at `0x5127b28` (`CD CC CC 3D`).
  - Otherwise `err *= OFF`. OFF = 0.6 at `0x5127b2c` (`9A 99 19 3F`).
  - Then err is clamped to ±0.5 m (`0x4926344`/`0x4926348`), and `+0x1dc = base + err`.
- **Drawn model:** the model-matrix builder `0x15132e0` translates the model along its up axis by weight × `+0x1dc`
  (`0x1513406`). Lua `SetFootIKInterpolateType` (`0x1338a10`) sets the weight (`+0x258`/`+0x25c`).
- **At 120 FPS:** both gains act 4× faster than on PS4, so a transient IK error at an animation blend moves the model
  further and faster. No patch touches this.
- **Scope:** characters only (the Doll c8030, the bath messengers c8050). The note and bloodstain messengers
  (SprjObjMessenger) have no ChrCtrl.
- **Checked by hand:** the two readers are `0x1515182` (`vmulss xmm0, xmm2, [rip+0x3c1299e]` → `0x5127b28`) and `0x1515195`
  (`vmovss xmm0, [rip+0x3c1298f]` → `0x5127b2c`). `+0x1dc` is reset each frame (`0x151501d`/`0x1515059`) before err is
  added (`0x15151e8`). The globals are in the RW segment (vaddr `0x50dc000`+, file offset = vaddr + 0x4000).
- **Fix (`src/runtime_ik.c`):** all five sites (the update entry, both gains, both readers) are byte-checked first. Then a hook
  on the ChrCtrl::Update entry writes `ON = 1 − 0.9^(30·dt)` and `OFF = 0.6^(30·dt)` from that call's dt before every
  character update. The gains are computed from the game's constants with no shared state, because characters may update
  on several threads.
- **Frozen characters:** a character in hit-stop gets dt 0, so on = 0 and off = 1, and its correction pauses with it.
- **Debug menu:** with "Restore Debug Menu", the menu's two gain items point at the globals, and the hook overwrites any edit
  at the next character update.
- **Switches:** `BB_IK_HEIGHT=0` (or `off`) sets both gains to 0, which turns the correction off (a diagnostic; feet may float on
  slopes). `BB_IK_HEIGHT=vanilla` keeps the per-frame gains. `tests/test_ik.c` covers this.

## Candidate 2: our Uncap FPS++ is out of date
- **What changed:** shadps4-emu/ps4_cheats commit `87bdf80` (2026-10-10) added the timestep fixes from
  yogesh1239/BB-Sprint-Fix-Patch (`docs/TIMING_FIX.md`) to all the FPS++ patches. It also added a 120 FPS++. Our
  `patches/Bloodborne.xml` (and deadinside28/bloodborne_pc master) predates it.
- **Missing hooks:** `0x01915FAB`, `0x01916030`, `0x01915F71`, `0x016A4416`, `0x01C0F647`, `0x0183E2B2`, `0x01E4ABCA` and
  `0x02713D76` (XML addresses).
- **Relevant fixes** in that commit:
  - the clip/track player `0x1915ec0` (XML) had a fixed 1/30 step, so "clip motion near enemies ran too fast";
  - the ragdoll fade-in;
  - "value smoothing";
  - 19 timers that ran 2–8× short.
- **Still open there:** "physics-like data (`0x2c7807a`, `0x2cbc4b3`) changes 1.3–1.9× between 60 and 120 FPS".
- **Broken edit:** Paleblood's QUIRKS.md reports that the Uncap FPS++ edit at `0x02715D71` (`call 0x027166f7`, grass
  sway) jumps into a static initialiser; our file has the same bytes.
- **Updated 2026-10-11:** `patches/Bloodborne.xml` now has ps4_cheats' Uncap FPS++ and 120 FPS++ as of commit `e3446394`
  (2026-10-10 19:38, the follow-up to `87bdf80`, with two more camera hooks). The Sprint Fix is built in.
  - **Left out:** one line, the broken `0x02715D71` call. Its rel32 is overwritten by the next line, and the code is dead
    after the `0x02713D76` rewrite.
  - **Switches:** `BB_FPS_PATCH=old` selects the previous version, kept as "Uncap FPS++ (2026-08-28)" with the separate
    Sprint Fix. `BB_FPS=120` applies 120 FPS++ (fixed timestep, vblank 120).
  - **Checked** (`tests/test_patches.py`): no overlap with the other uncap-preset patches or with any byte-checked
    `src/runtime_*.c` site. Its caves (`0x05490000`–`0x05490AC5`, `0x054A0000`–`0x054A00F3`) lie in the zero run at the end
    of the text segment, which nothing else uses.
  - **Camera:** it scales four of the camera chase rates itself (see `docs/camera_re.md`).
  - **Still at the 2026-08-28 versions:** 60 FPS++ and 90 FPS++.
- **Not reported anywhere:** no public report of the dip itself was found (shadPS4 issues, the compatibility repo, bbport
  #1–#149, Reddit, Paleblood).

## Ruled out or unlikely
- **The anim player blend** `0x18438b0`, shared by chr and obj: it is dt-scaled (smoothstep weight).
- **The messenger state machine:** none of its 46 steps has a height offset or a per-frame step.
- **The port's GPU layer:**
  - Bone palettes (≤ 16 KiB vertex buffers) are snapshotted in stream order on the GPU thread.
  - EOP labels are honest under GuestInPlace.
  - The motion-vector shader variant never writes positions.
  - The VRAM-copy weak spot (GarlicInVram) needs a discrete GPU, and the Steam Deck is integrated.
  - FSR 4 could still smear a frame from mismatched messenger history (several identical draws).

## A/B tests (same spot: the Doll, open Level Up)
1. `BB_FPS_LIMIT=30` against the default 120. If the dip shrinks or slows, it is frame-rate dependent (candidates 1/2).
2. `BB_UPSCALER=off`, and `object_motion=0` in bbport.ini. If the dip goes away, it is the upscaler's history, not geometry.
3. `BB_PC_MODEL=0`. If the dip goes away, it is the memory model.
4. Then the switches for candidates 1 and 2, once they are coded.

## Foliage speed (2026-10-12)
- **Report (Asad):** with the current Uncap FPS++ (465bcf7), flowers in the Hunter's Dream move "way faster than they
  should", which reads as flickering, even when the player stands still. It was not there with the 2026-08-28 patch.
  Normal launcher: uncapped, 120 FPS limit, 2160p output.
- **Frame time:** `[[0x059404F8]+0x264]` is SprjFlipper's wall-clock frame time in seconds (`0x2434CF0`: µs difference /
  1e6). The patch's caves clamp it to 1/30. `0x02418E3D` also stores the clamped value to `0x0575A908` every frame,
  so `0x0575A908` holds min(max(dt, flipper+0x18), 1/30); `+0x18` is 0 under the patch. The 1/120 written there is
  only its first value.
- **Frame mode flag:** `[[0x59406C8]+0x100]` copies SprjFlipper+0x10, the flip interval (2 = 30 FPS, 1 = 60 FPS).
  Both patches force 1.

### The wind/foliage constants function `0x2713870`–`0x27165EE`
- **Callers:** two render-instance vtables (owners built near "CSLod"/"RendMan"). Its output is 28 float4s packed
  into per-instance shader constants (`0x2126B60`).
- **What it does:** it reads wind sources (Perlin fBm, a jittered direction).
  - It integrates per-slot timers, age and fade with one step: age in `.w`, a 0.1 s fade-in, a 0.75 s lifetime.
  - Every frame it rebuilds the offsets `disp = step × velocity` and `dist = step × (speed + |random delta|)`
    (`xmm14`, at `0x2714A66` and `0x2714AE7`). These are not integrated.
  - There are no "grass"/"wind" strings, so the label is inferred.
- **Steps by version:**
  - vanilla: 1/30 a frame;
  - both patches: flag 1, so 1/60 a frame. At 120 FPS the timers run 2× in the old patch;
  - current patch: the clamped frame time for both (`0x02713D76`, and `0x02713DB1`/`DBB` read `0x0575A908`). The timers
    run in real time. The offsets shrink as FPS rises and change size with every frame-time change.
- **Candidates:**
  - `BB_FPS_SKIP=grass`: back to 1/60 a frame;
  - `BB_FPS_TRY=wind30` or `wind60`: a fixed offset step of 1/30 or 1/60, while the timers keep the frame time
    (`0x027149EA`, `0x02714B26`).

### Other lines of the current patch that can make something faster or unstable
- **`0x01972A7D`:** the update context `0x1972900` passes to `0x21145B0(obj [r12+0xD0])` carries 1/30, the game's
  value. The old patch had 1/240. It runs only when `[r12+0xC4]==1`, from one caller (`0x1D3AC10`). If that runs once a
  frame, it is 4× real time at 120 FPS; how often it runs was not checked, and the system is not identified. Candidate: `BB_FPS_TRY=step1972`, which uses the clamped frame time (cave at `0x05490B00`).
- **Havok Cloth (`cloth`):** stiffness × (min(dt,1/60)·60)², sub-steps forced to 1 (`0x00B6E67A`). The old patch
  left cloth alone, and wind on plants can be cloth (`hclSimpleWindAction` is in the binary).
- **`sfx`:** the SFX frame rate goes 30 → 60 (`0x026FB39F`; frame counts / rate = seconds). It may run 2× unless
  something compensates. `fps60` holds the other "per second at 30" values that were doubled.
- **`hknp`:** the default solver step 1/30 → 1/120 (`0x00F383CA`). The old patch's line hit padding at `0x011383CA`
  (the 30/60/90 FPS++ blocks still carry that typo).
- **`timeobj`** (`0x2677780` → `0x12BBDA0`) and **`clip`:** real time now (the old patch was 2× or 4×), so they are
  not "faster".
- **Not grouped:** camera, sleeps, smoothing and the 1/30 → 1/60 task contexts: character, thread or camera code,
  slower than the old patch.
- **Frame limiter:** `[+0x18]` = 0 removes the game's 240 FPS cap. Only `BB_FPS_LIMIT=0` exceeds it.

### A/B (Hunter's Dream, by the flowers, standing still)
`~/.local/bin/bloodborne-ab.sh ENV=VALUE...` (log `out/play-ab.log`):
1. no settings (baseline);
2. `BB_FPS_PATCH=old`;
3. `BB_FPS_LIMIT=30`;
4. `BB_FPS_TRY=step1972`;
5. `BB_FPS_SKIP=cloth`;
6. `BB_FPS_TRY=wind60`;
7. `BB_FPS_SKIP=sfx,fps60`;
8. `BB_FPS_SKIP=hknp`.
