# Messengers: reverse-engineering notes (v1.09)

Image offsets, as in `docs/respec_re.md`. This is static analysis from 2026-10-11; the game was not run. The symptom: messengers
"vanish at odd distances, everywhere" (Asad; 2160p output, FSR 4, uncapped at ~240 FPS).

## The renderer
- No draw is culled by distance.
- Occlusion queries are faked as always visible (`liverpool.cpp`, ZPASS_DONE).
- Object-motion selection never skips a draw.
- Readbacks are rare (0.6–0.8/s).

## What the messengers are
- **Notes, bloodstains and summon signs:** `SprjObjMessenger` objects (models `o000200`–`o000260`, about 0.8 m,
  animated), with the state machine `SprjObjMessengerState` (steps registered at `0x1ccaa00`): Underground →
  Appear → Near/Far Idle → Disappear.
- **The bath messengers** are the chr `c8050`.
- **Lamp messengers** show by flag (`+0x144` bit 0x10), not by distance.

## Hide and emerge
- **The rule:** "player is near" (`0x1cc6a30`) tests squared distance < 25.0f (`0x4928b50`, read only at `0x1cc6afa`).
  Messengers come out within 5 m and sink beyond it. That is vanilla behaviour, and the likeliest thing Asad sees.
- **Precision:** every distance test there is exact (`vsqrtss`, squared distances), and every timer is scaled by dt. No Intel
  approximation (`vrsqrtps`/`vrcpps`) or per-frame step was found on this path.
- **Diagnostic:** `BB_MESSENGER_RADIUS=<metres>` (`src/runtime_messenger.c`, byte-checked) changes the radius. If the
  vanishing distance follows it, the vanilla rule is the cause.

## LOD
- **How it's chosen:** CSLod (`0x1d41d60`) uses exact distance × the camera's lodScale, with hysteresis.
- **The models:** every messenger model has real LOD1/LOD2 geometry, so LOD can make them coarser and pop, but not vanish.
- **lodScale and render height:** lodScale scales with 720 / render height (`0x2292017`), so lower presets pop sooner. Asad's
  preset renders at ~1080p, about the same as the PS4.
- **The patch:** "Model LOD -2" (`model_lod=-2` in bbport.ini) keeps LOD0 always.

## A/B in the test phase
1. `BB_MESSENGER_RADIUS=20` against unset, at a note.
2. `model_lod=2` against `-2`: messengers vanishing up close with LOD2 would mean the port mis-draws LOD face sets.
3. `BB_FPS_LIMIT=30` against 120: does the emerge distance stay the same?
