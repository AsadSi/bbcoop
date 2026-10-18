# DualShock motion (gyro and accelerometer): notes (v1.09)

Asad asked for this on 2026-10-18. On a PS4, the game does something with the pad's motion while X is held. He
remembers it as a shortcut for gestures. The port's `scePadReadState` always said "at rest". `src/runtime_padmotion.c`
now fills the motion fields from SDL3's gamepad sensors. `BB_PAD_MOTION=0` turns it off, and `BB_PAD_MOTION_TRACE=1` logs
the values once a second while the pad turns.

All addresses are image offsets. The pad imports are resolved through the dynamic tables: `scePadReadState`
GOT 0x53e4ea0, stub 0x2bbf4d8.

## What the game imports
- `scePadReadState`, `scePadResetOrientation`, `scePadSetAngularVelocityDeadbandState` and `scePadSetTiltCorrectionState`.
- Not `scePadSetMotionSensorState`: motion is on by default on a PS4.
- Orientation reset, deadband and tilt correction only matter for the orientation, so the game probably uses it as
  well as the raw values.

## The pad device: 0x2690090 (and its twin 0x2691870)
- It reads `ScePadData` into the device object at +0x110:
  - +0x11c: orientation (x, y, z, w).
  - +0x12c: acceleration.
  - +0x138: angular velocity.
- It then fills 22 analog channels at +0x1a4, which the getter at 0x268fff0 (vtable 0x52b8330, slot 0xd0) reads by
  index:

| Channels | Device field | Contents |
|---|---|---|
| 0–3 | | the sticks |
| 4–5 | | L2 and R2 |
| 6–8 | +0x1bc.. | acceleration, clamped to ±3 and divided by −3 |
| 9–11 | +0x1c8.. | angular velocity, clamped to ±6.5345 rad/s and divided by −6.5345 |
| 12–15 | +0x1d4.. | the orientation, copied as is |
| 16–21 | | two touches |

- The ±3 clamp means the game expects g, not m/s²: at rest, 9.8 would be clamped and stuck at the limit. So the port
  divides SDL's m/s² by `SDL_STANDARD_GRAVITY`.
- shadPS4 passes SDL's m/s² unchanged, which would clip here.
- The code that turns the channels into a gesture reads them through the shared virtual slot, mostly in loops, and was
  not traced.

## What the port sends
- **Acceleration:** in g.
- **Angular velocity:** in rad/s, as SDL gives it.
- **Orientation:** the angular velocity integrated since the last `scePadResetOrientation`, from the identity. Steps
  over 0.1 s are skipped.
- **Not modelled:** tilt correction and the gyro deadband.
- **Axes:** SDL's (x right, y up, z toward the player), assumed to be the PS4's. Not verified against a real PS4.
- Checked on Asad's DualShock 4 (USB, SDL3 outside the game), lying flat: accel (−0.13, 9.60, 1.87) m/s², which is
  (−0.01, 0.98, 0.19) g, and the gyro within ±0.003 rad/s.

## Verified in game (Asad, 2026-10-18)
- Holding X and moving the DualShock 4 works as on the PS4.
- If a later change breaks it, suspect the signs of the axes first. `BB_PAD_MOTION_TRACE=1` shows the values.
