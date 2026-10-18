# bbcoop — brief

Started 2026-10-07. Approved plan: `~/.claude/plans/i-was-considering-making-crystalline-reddy.md`.

## What this is

A private fork of bbport (deadinside28/bloodborne_pc) that adds seamless co-op for a small group of friends. One player's PC hosts the session and the others join over Tailscale. It also adds co-op enemy tuning, shared loot, and a respec option at the Doll that costs Insight.

**Done** means two friends on Linux PCs can:
- beat Father Gascoigne together without being sent home,
- travel by lamp together,
- both receive every pickup and boss drop,
- respec stats at the Doll, or refund a weapon's materials at the Workshop's workbench.

## Not building

- Our own runtime or recompiler. bbport already runs the game's x86-64 code natively, and forking it gives us full control.
- A public server, matchmaking with strangers, or any connection to real PSN.
- PvP or invasions.
- Chalice Dungeon co-op.
- Windows builds.
- Support for any game version other than v1.09 of CUSA03173 (EU) or CUSA00900 (US).

## Decisions made

| Decision | Choice | Why | By |
|---|---|---|---|
| Base | Fork of bbport | Recompiling ourselves would rebuild bbport for no gain, since the mods need RE of game logic either way | Asad |
| Hosting | The host player's PC runs the server inside its launcher, and friends connect over Tailscale | No separate server to maintain | Asad |
| Audience | Private friend group, private GitHub repo | GPL: everyone who gets a build also gets the source | Asad |
| Party size | Host plus 2 guests (the game's own limit) | Going above the native limit means more RE and more desync | Asad |
| Guest death | Respawns at the host's last lamp and is re-inserted | Seamless Co-op model | Asad |
| Host death | The host respawns at their lamp and guests stay where they are, with no reset for them | Asad: the host dying must not send everyone back | Asad |
| Progress | Boss kills, lamps and shortcuts are copied into each guest's own save | Seamless Co-op model | Claude |
| Non-co-op online traffic | Public servers allowed: the game's own server traffic (messages, bloodstains, ghosts) may go to The Hunter's Dream, accounts and summons to srv.shadps4.net or the group's own shadNet server. Replaces "private only" | "It doesn't have to be private, just make online work" | Asad, 2026-10-12 |
| Online code | shadp2p's C++ NP, Net and Http libraries (about 25k lines), adapted into a C++ library linked like the GPU library, with no rewrite into C. Tested by a fake-game client that makes Bloodborne's online calls against the real server | A C rewrite would take 7-9k lines plus a protobuf C port, for no gain | Claude |
| Loot | Every pickup, enemy drop and boss drop is mirrored to each player | "Fair for co-op" | Asad |
| Difficulty | +50% HP and +10% damage per extra player (BBSC's scaling with these numbers). No extra enemies | "No need for more enemies" (2026-10-17) | Asad |
| Respec | In the Hunter's Dream, outside a session. "Reset Attributes" (at the Doll) returns level and stats to the origin's and refunds every echo spent on levels, for 1 Insight (`BB_RESPEC_FREE=1`: free). "Refund Weapon Upgrades" (at the workbench, just before Leave; moved from the Doll 2026-10-12, Asad) opens the Fortify list of upgraded weapons, +10 and firearms included. Picking one sets it to +0 for free and returns every upgrade material, with fitted gems going to the inventory. The echoes spent on upgrades are lost. Saves a backup first. Built and verified in game 2026-10-10 (`src/runtime_respec.c`, `docs/respec_re.md`) | | Asad, 2026-10-10 |
| Respec entry | A new option in the Doll's dialogue (her talk script), not a settings-menu button | Feels native; chosen over the faster menu button | Asad, 2026-10-09 |
| Seamless engine | Upstream 0.6's BBSC (`BB_BBSC=1`, the online module) is the seamless co-op, run with bbcoop's numbers (BBSC_SCALE_ENEMY_HP/BOSS_HP 50, DAMAGE 10 per extra player, BBSC's stagger) and bbcoop's mods (respec, gun gems, camera, IK). bbcoop's own seamless patches and hooks (`BB_SEAMLESS=1`) stay as the fallback and are skipped while BBSC is on: both patch the same sites. BBSC already has scaling, guest items and boss rewards, shared progress, lamp warps together, up to five hunters, the host-death rule and respawn in place | "Take the best of both" | Asad, 2026-10-17 |
| Frame rate | 0.6's frame-rate hooks (48 per-frame values follow the frame time, running fixed), with bbcoop's camera chase rates and foot-IK correction on top (no shared sites). bbcoop's Uncap FPS++ patch stays selectable with `BB_FRAME_HOOKS=0` | "The best one, a mix of both" | Asad, 2026-10-17 |
| Seamless patches | Go through `src/runtime_seamless.c`, which checks the original bytes, not bbport's XML loader | The XML path writes without checking, so a wrong address would silently corrupt the game | Claude |
| Our code | In new `src/runtime_*.c` files, which `build.sh` already compiles, with only small marked edits to upstream files | bbport changes every day, so rebasing must stay cheap | Claude |
| US edition | CUSA00900 v1.09 is supported alongside CUSA03173 v1.09, with no per-build offset table | Its eboot.bin loads to the same image as EU 1.09 (SHA-256 071df19c…4c0a), so every offset and import is the same. Saves are per title ID, and the NP comm ID is region-specific (US NPWR05818_00) | Asad (request); checked 2026-10-09 by Claude |
| macOS | An x86-64 build of the same runtime, run by Rosetta 2, with MoltenVK for Vulkan; since 0.6 upstream's port (PR 191, bbport `macos-0.6`). Replaces the shelved static recompiler (branch `recomp`) | macOS 27 still ships Rosetta in full (Apple keeps a games subset after it), and it runs every Jaguar instruction the game uses (AVX, BMI1, MOVBE, LZCNT, F16C; checked 2026-10-12) when `ROSETTA_ADVERTISE_AVX=1`. Days of porting instead of months of lifter work, and the co-op code stays one form | Asad (request), Claude (route), 2026-10-12 |
| Addresses | shadp2p offsets are already bbport image offsets (an XML address minus 0x400000, as shown by `memory_patcher.cpp:282` and `scripts/patches.py:197`) | Checked 2026-10-07 | Claude |

## Reusing

- **bbport** (this repo) provides the loader, runtime, renderer, mod overlay (`scripts/mods.py`) and patch compiler (`scripts/patches.py`).
- **shadp2p** (`../shadp2p-ref`, GPL-2.0+):
  - `src/core/bloodborne_re.cpp` has the seamless patches with their original and new bytes, plus the native function offsets (lines 104–188).
  - `src/core/libraries/np/np_matching2`, `np_signaling`, `np_web_api*` and `src/shadnet/` are the NP client.
  - `documents/bloodborne-seamless-re.md` and `README-bloodborne-internals.md` are its RE notes.
- **shadnet-p2p** (`../shadnet-p2p-ref`, GPL-2.0+, C++) is the matchmaking, signaling and WebAPI server, to be embedded in host mode.
- **Not reusable:** `~/Documents/code/oot-recomp` (N64Recomp) targets a different CPU architecture and only overlaps in general RE habits.
- **Modding tools:** Smithbox, Paramdex BB, WitchyBND and DarkScript3 for the param, map, event-script and talk-script edits.

## Shape

There are three layers, each with one owner:
1. **Game networking:** `src/runtime_net.c` and `src/runtime_np.c`. These are the PS4 Net, NetCtl, Http and NP functions that the game calls, ported from shadp2p. The game's own netcode stays in charge of summoning, insertion and combat sync.
2. **Mod channel:** `src/runtime_coop.c`. Versioned messages, sent through the host, for what the game doesn't sync: lamp travel, event flags, item lots, echoes, deaths and enemy resets. This is the only place where co-op rules live.
3. **Data mod:** `mods/coop-rebalance/dvdroot_ps4/...`, loaded through bbport's existing loose-file overlay.

Supporting pieces:
- **Hooks:** `src/runtime_hooks.c` (M1) checks the expected bytes and installs absolute-jump trampolines into C handlers.
- **Respec:** in `src/runtime_coop.c`, triggered by an event flag that the Doll's talk script sets.

## Screens

- **Build now:** a launcher Host/Join panel. The host sees "share this IP and code"; a guest enters the host's IP and code.
- **Later:** an in-game session overlay showing who's connected.

## Reference values (to check against v1.09's own tables)

- **Level cost above level 12:** the community model is 0.02x³ + 3.06x² + 105.6x − 895 Blood Echoes at hunter level x (Fextralife; gamerguides.com). Respec refunds should use the game's own cost function once it's located, not this formula.
- **Upgrade costs:** 3/5/8 Blood Stone Shards for +1 to +3, 3/5/8 Twin Shards for +4 to +6, 3/5/8 Chunks for +7 to +9, and 1 Blood Rock for +10 (gamerguides.com). A December 2015 patch changed upgrade costs, so refunds read EquipMtrlSetParam from the game instead of using this table.

## Open

- **Mac speed and image (2026-10-17, MacBook Air M4):**
  - Turn Low Power Mode off: with it on the game ran at ~20 FPS uncapped instead of ~43–45.
  - The window now uses the Retina display's pixels (it was 1470x816 at scale 1, shrunk then doubled:
    soft and out of shape). Fullscreen (launcher setting, F11) is 2940x1846, 16:10 below the notch.
  - Passes with no output use a 16x16 area (`BB_NO_OUTPUT_AREA=0` off); command buffers are unnamed on macOS.
  - Measured and not taken: MoltenVK prefill (no gain, more late frames), `BB_COPY_SHADER_CPU=1` (more late
    frames), `MVK_BB_UNTRACKED=1` (0 late frames once, 240 the next run: noise).
  - 60 FPS is CPU-bound on the Air: uncapped ~43 FPS at Quality and at Performance alike, the GPU thread idle
    57–69%. Wooloomooloo2/bloodborne_mac_port reaches 60+ with a native arm64 GPU process; not done here.
  - At 30 FPS some frames show 50 ms then 17 ms (judder). `BB_SWAPCHAIN_IMAGES=3` may help; inconclusive.
  - The intermittent "PM4 type 0" crash on loads (2 of ~14 loads today) is a game race between the main
    thread's GPU setup (0xebb960) and GXRenderThread's frame job (0x2199af0) on one immediate context;
    Wooloomooloo2/bloodborne_mac_port 12a0ac5 serialises them with one lock; ported 2026-10-18
    (`src/runtime_context_lock.c`, `BB_CONTEXT_LOCK=0` off). Installed and played at the Dream; loads with
    the lock not yet counted.
- **Mirrored loot on BBSC (2026-10-17, merged):** every pickup, enemy drop and boss drop goes to every hunter
  (`gpu/bbnet/shadps4/core/bbcoop_loot.inc`, relay "SHAD" 0x28, `SHADPS4_BBSC_MIRROR_LOOT=0` off,
  `docs/coop_loot_re.md`). Verified alone with its test seam (receive side); a real pickup's send side and a
  real session are not.
- **BBSC packets 0x23..0x27 (2026-10-17, fixed):** life, scaling, PvP, shared progress and the boss bar were sent
  but dropped by the receive thread (upstream 0.6-pre1 too), so a guest never got the host's scaling or
  progress. Now handed on. Needs a real session to see end to end.
- **Guest death at the host's lamp (2026-10-17):** BBSC respawns a guest at the host's lamp only when that
  lamp is loaded on the guest's side; bbcoop now loads the guest there otherwise (outside boss fights,
  `SHADPS4_BBSC_DEATH_TO_HOST_LAMP`, `docs/coop_respawn_re.md`). Verified alone with the test seam; the
  session-keeping part needs a real two-player session.

- **Character editor invisible/white/unchanged (Asad, 2026-10-13), USABLE.** Cause: Uncap FPS++'s frame time can be 0 when frames are presented back to back (Havok Cloth and character code divide by it); the default had the model stay unchanged on skin/gender changes. Fix: `dtfloor` (`BB_FPS_TRY`, now default; 1/240 floor, `patches/Bloodborne.xml`). Verified by Asad: changes apply. Remaining: the model flickers invisible/visible on every second change, accepted as fine. Not the cause: `BB_GPU_WRITE_TWINS_MAX`. Unfinished bisect over Uncap FPS++ units (0:81 dropped = visible; culprit among units 40-59, 70-74, 78-80) needs a rerun without other game launches (each step `pkill`s bb-probe). Build: `build.sh` skips the probe compile when unchanged (`BB_REBUILD=1` forces); a corrupt out/gpu/.ninja_deps (killed builds) made ninja recompile 180 files on every launch (58 s -> 0.7 s after deleting it).
- **Online and co-op are on hold (Asad, 2026-10-11).** Plan: `~/.claude/plans/make-a-plan-on-scalable-kurzweil.md`.
  - **Done:** the launcher's Host/Join, the server, the ported NP libraries, and the seamless patches and hooks.
  - **Missing:** `src/runtime_coop.c` (lamp sync, mirrored loot, flag sync, death rules), HP/damage scaling, and a real two-player session.
  - **Blocker:** the reply to `/blood_messenger/exist_messages`, below.
  - **Commits:** kept local, not pushed (Asad, 2026-10-11).
  - **Upstream 0.51** was merged on 2026-10-11, bringing atomic saves, stutter and crash fixes, the System-menu settings and DLC. Its online module `gpu/bbnet` replaced our vendored copy of the same shadp2p libraries, which is kept at the tag `bbcoop-online-0.4`.
  - **To port into `gpu/bbnet` when online resumes:**
    - `BB_SEAMLESS` acceptance and `BB_CAPTURE_ONLINE` in `http.cpp`
    - the co-op relay: `np_handler` `onCoopRelay`, the shadNet client and proto relay, `bbcoop_sync.cpp`
    - the NpLookup success stub
    - `bbcoop_set_game_info`
    - pointing its placement code at libbbgpu's `bbcoop_placement_*`
    - the end-to-end test (`tests/online_client.cpp`, `scripts/test-online.sh`)
    - the launcher's Host/Join tab (`launcher/bbcoop_online.py`, now unloaded) against upstream's Online page
  - **Seamless co-op with upstream online** will need `BB_SKIP_NETWORK_CHOICE=0`.
- **Camera speed-up and messengers (Asad, 2026-10-10/11):**
  - The camera suddenly turns faster.
  - Messengers vanish at odd distances, everywhere.
  - Asad's settings: uncapped frame rate, 2160p output, FSR 4. The log shows the game at ~240 FPS.
  - The renderer drops no draws by distance, and occlusion queries are faked as always visible. Both bugs point at game logic that counts per frame above ~120 FPS, like the wall detector behind "Sprint Fix (High FPS)".
  - Uncapped now defaults to a 120 FPS limit (`run.sh`, launcher). `BB_FPS_LIMIT=0` removes it.
  - **Camera:** the game's own turn ramp (0.6 s at full stick, 120 to 260°/s) is off, and the 30 FPS chase rates follow the frame time (`src/runtime_camera.c`, `docs/camera_re.md`).
  - **Messengers:** the notes' and bloodstains' messengers emerge within 5 m and sink beyond it, by design. Static analysis found no frame-rate, Intel-math or renderer cause (`docs/messengers_re.md`). `BB_MESSENGER_RADIUS` tests it in the game.
- **Firearm gem slots (Asad, 2026-10-11):**
  - Every firearm already fortifies to +10.
  - The Cannon, Church Cannon, Rosmarinus, Flamesprayer, Fist of Gratia and Hunter's Torch get the same single gun-gem slot from +1 that the pistols have, through `tools/respec_mod.py`. Its shape is Droplet, except Triangle on the Flamesprayer, Rosmarinus and Hunter's Torch and Radial on the Fist of Gratia (2026-10-17).
  - Shields and the Torch stay as they are.
- **Fire Paper, Bolt Paper, Empty Phantasm Shell (Asad, 2026-10-18), verified in game and accepted by Asad:**
  - Instead of a flat +80 per hit, the bonus is 80 × (base at +0 × the attack's multiplier) / 90 + 10% × (base at the current level × multiplier).
  - It keeps the early-game punch and adds a late-game share, and no longer favours fast weapons.
  - Viscerals still get nothing, and stats and gems don't change it.
  - `src/runtime_papers.c`, `docs/papers_re.md`; `BB_PAPERS=vanilla` off, `BB_PAPERS_TRACE=1` logs each bonus.
  - Tested with the Shell on Ludwig's Holy Blade +10 and the Threaded Cane +10: the trace matched the formula, the status screen shows the new value, and viscerals got none. Against `BB_PAPERS=vanilla`, the transformed R2 did a little more damage.
- **Verified in game 2026-10-11** (throwaway profile, `BB_TEST_ENDGAME`, 2160p, FSR 4, 120 FPS cap):
  - **Camera:**
    - With a full stick, turning stays at ~120°/s.
    - With `BB_CAMERA_ACCEL=1`, the vanilla ramp reaches 1.0 within about 0.5 s and turning jumps to ~270°/s.
    - The chase rates follow dt (`BB_CAMERA_TRACE=1`).
  - **Gun gems:**
    - The Cannon +1 and Flamesprayer +10 show 0/1 slots in Fortify and in Blood Gem Fortification.
    - Refunding the Flamesprayer at the Doll returned 16/16/16/1 materials.
    - The Workshop workbench's own Fortify then upgraded it normally to +1 (normal confirm text, Echoes and Shards spent), and the slot was back.
  - **Stat reset:** 150 → 10 for 1 Insight.
  - **Respec menu:** works after the upstream merge.
  - **Frame rate:** 108–117 FPS, worst frame ~16–22 ms.
- **Verified by Asad 2026-10-11:** a gem fits in a new gun slot. He used the gem-test save (`out/gem-test`, from `BB_TEST_ENDGAME`; launcher `~/.local/bin/bloodborne-gemtest.sh`).
- **Still not verified in game:**
  - A refund with a full inventory or storage. Gems and materials then go to storage. A gem with no room anywhere stops the refund, and the weapon stays as it is.
  - Whether the messengers Asad sees vanish by the 5 m rule (`BB_MESSENGER_RADIUS`).
  - The AppImage build does not ship `tools/respec_mod.py` or soulstruct, so the Doll's new options are missing there. Asad doesn't use the AppImage.

- **Online in the real game (2026-10-09, US 1.09, local server):** NP sign-in, `ss.info` (`/bb-us/ss.info`), `/basic_utils/login` and `/basic_utils/sync_chara_id` (needs `PublishCharacterIdList` of `PublishCharaId`) are accepted. Next blocker: the `BloodMessSearchAdd` response (`/blood_messenger/exist_messages`) is rejected ("Failed to record note search information") even with `LostBloodMessIdList: []`; the parser takes the array (type 6) at image 0x1e84f12, so the cause is elsewhere (ResKind meaning?). Response keys are UTF-16 in the eboot at file offset 0x49d0d40. Reach it with the title, main menu Log In, then Continue (the title remembers Play Offline).

