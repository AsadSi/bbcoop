# Camera at uncapped frame rates: reverse-engineering notes (v1.09)

Image offsets, as in `docs/bbcoop/respec_re.md`. The work was done on 2026-10-11 by static analysis only; the
game was not run. The symptom: the camera "suddenly speeds up" (Asad, 2026-10-10). Asad runs uncapped,
which the log shows at ~240 FPS.

## How the frame rate reaches the game
- **Patches:** `fps_mode uncap` applies "Uncap FPS++" (`scripts/patches.py`). Since 2026-10-11 that is ps4_cheats' version
  of 2026-10-10, with the Sprint Fix built in. `BB_FPS_PATCH=old` selects the previous one with the separate "Sprint Fix
  (High FPS)".
- **Frame timer** `0x2034770` (object `*(u64*)0x55404f8`):
  - It targets 1/240 s with a `sceKernelUsleep` limiter (`0x2034b90`), so the game never runs above 240 FPS.
  - `[+0x264]` is the raw `gettimeofday` delta.
- **Main loop** `0x2018e36`: the task frame time is `clamp([+0x264], 1/240, 1/30)`. Below 30 FPS the whole game,
  camera included, runs in slow motion. It "speeds up" again when the frame rate recovers.
- **Raw frame time** reaches three sites unclamped: `0x17f9ca9` (min 1/60), `0x1dbc181` and `0x1f77cec`.
  A wall-clock step became one huge frame there. `src/runtime_time.c` now serves `gettimeofday` from
  the monotonic clock; `BB_WALL_CLOCK=1` restores the wall clock.

## The follow camera (ChrExFollowCam)
- **Update:** `0x143ac60(cam = rdi, …, dt = xmm0)`, called from `0x1436c00` and `0x1436e15` with
  `rdi = [rbx+0x60]` and xmm0 from `[rbp-0x40]` (`0x1436b34`). The caller also divides by it
  (`0x1436baa`), and the update multiplies speeds by it (`0x143b069`) and uses it for the turn ramp below.
- **Defaults:** two init blocks, `0x143a0e0`-area immediates at `0x143a57b…` and `0x143a979…`.
- **Field names:** from the debug-menu builder `0x143fb60`.
- **Manual turning is dt-correct:** CamRotX/Y (`+0x140/+0x144`) += stick × speed × dt.

### The turn ramp (vanilla)
- **Trigger:** stick magnitude > 0.998 (`0x143cc1b`) with |yaw| > cos(DirectPitchAng).
- **Ramp:** RotHiSpeedRate `+0x138` += dt / RotHiSpeedRateIncTime (`+0x1e8` = 0.6 s; `0x143cd22`, `0x143f7d0`), up to ±1.
- **Speed:** lerp(RotSpeed ≈ 120°/s, RotHiSpeed ≈ 260°/s, |rate|).
- **Reset:** a stick below 0.998 resets it at once (`0x143cd5e`).
- **Effect:** a full stick for 0.6 s more than doubles the turn speed.

This is most likely what Asad sees. Measured in the game on 2026-10-11:
- With the vanilla ramp, RotHiSpeedRate reaches 1.0 within about 0.5 s of a full stick, and turning goes to ~270°/s.
- With the ramp off, turning stays at ~120°/s. `src/runtime_camera.c` sets `+0x1e8` to 1e9 s in both init blocks, so the
camera keeps one speed (`BB_CAMERA_ACCEL=1` restores the ramp).

### Per-frame chase fractions (tuned for 30 FPS)
`cur += (target − cur) × rate` once per frame, with no dt:

| Field | Name | Default | Read at |
|---|---|---|---|
| `+0x240` | LockRotChaseRate | 0.3 | `0x143c30f`, `0x143c784` |
| `+0x1a8` | TargetRotXChaseRate | 0.3 | `0x143cef0` |
| `+0x23c` | DirectPitchRate | 0.06 | `0x143f97e`, `0x143fa9f` |
| `+0x288` | lock-adjust rate | 0.3 | `0x143c9e6` |
| `+0x330` | LockCamParam interpolation | 0.05 | `0x143ae75` |

- **Effect:** at 120 FPS lock-on and pitch return converge about 4× faster than on PS4.
- **Fix:** a hook on the update entry (`55 48 89 e5 41 57 41 56 41 55`) rescales each fraction to
  `1 − (1 − rate)^(dt·30)`. That equals the game's value at 30 FPS. `BB_CAMERA_RATES=0` turns the hook off.
- **Base values:** the hook keeps the game's own value per camera as the base. A value it didn't write itself
  becomes the new base.

Left alone:
- **ChrTransChaseRate** `+0x198`: the game rewrites it every frame (`0x143e27f`, `0x143e2c2`).
- **The CamDist lerp constant** `0x4925e68`.
- **A 32-frame position average** in `0x1439430`.
- **The previous Uncap FPS++ and DirectPitchRate** (`BB_FPS_PATCH=old`): that patch sets DirectPitchRate to 0.03 (half
  of vanilla, for ~60 FPS) in the parameter builder (`0x1440799`, called from `0x14366a0` for the same camera). The hook
  reads 0.03 there as the vanilla 0.06, so the rate isn't halved twice.
- **The current Uncap FPS++ (2026-10-10)** scales four of the five chase rates by the frame time itself.
  - **Values:** it sets `+0x23c` to 0.04, `+0x240` to 0.15 and `+0x1a8` to 0.05.
  - **Reads:** it replaces the reads of `+0x240` (`0x143c30f`, `0x143c784`), `+0x1a8` (`0x143cef0`), `+0x23c` (`0x143f97e`,
    `0x143fa9f`) and `+0x330` (`0x143ae75`) with jumps to its own frame-time scaling.
  - **What the hook does:** `camera_install` runs after the XML patches. It leaves every field whose read is no longer the
    game's 9-byte `vmovss`/`vmulss [r13+field]` and rescales the rest; with the current patch that is `+0x288` only.
  - **Other patch edits:** it also hooks the `+0x198`/`+0x1c4` reads and changes `+0x19c`–`+0x1d0` and `+0x238`. The
    turn ramp (`+0x1e8`) is still ours.
- **The 60/90/120 FPS++ presets** halve several rates themselves at a fixed frame time, so `run.sh` turns the
  hook off for them (`BB_CAMERA_RATES=0` unless set).
