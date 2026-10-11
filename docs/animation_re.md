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
