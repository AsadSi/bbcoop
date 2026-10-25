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
| Progress | A guest plays in the host's world as in vanilla co-op: every area runs on the host's flags (doors, bosses, items taken, time of night; BBSC 0.0.2's WORLD_SYNC_ALL), and nothing of it is merged into the guest's own world (`SHADPS4_BBSC_SHARED_PROGRESS=0` in run.sh/run.py). On top of vanilla: warp together, no kick, shared pickups. Replaces copying boss kills, lamps and shortcuts into the guest's save | "Join his world like the usual Bloodborne online"; a host's door showed shut for the guest, and the guest's own doors opened in the host's world | Asad, 2026-10-25 |
| Chalice Dungeons | In scope (was "Not building"): the session drops when the host enters one. BBSC leaves area 29 out (all dungeons share the map ids); the guest needs the host's dungeon (glyph/layout), not its own. Next: a traced session (the BBSC timeline) to see where it drops | "Yes add it" | Asad, 2026-10-25 |
| Non-co-op online traffic | Public servers allowed: the game's own server traffic (messages, bloodstains, ghosts) may go to The Hunter's Dream, accounts and summons to srv.shadps4.net or the group's own shadNet server. Replaces "private only" | "It doesn't have to be private, just make online work" | Asad, 2026-10-12 |
| Online code | shadp2p's C++ NP, Net and Http libraries (about 25k lines), adapted into a C++ library linked like the GPU library, with no rewrite into C. Tested by a fake-game client that makes Bloodborne's online calls against the real server | A C rewrite would take 7-9k lines plus a protobuf C port, for no gain | Claude |
| Loot | Every player gets every pickup, enemy drop and boss drop once. Treasures in the host's world are first grab shares (2026-10-24): every hunter sees each one (BBSC's guest copies on); whoever picks it up first gets it, every other hunter gets a copy and their glowing copy disappears, taken in that world (`coop_loot_re.md`, Doubles). Other pickups are mirrored; enemy drops come from each player's own game (mirroring them doubled them, 2026-10-24); a guest's boss drop is BBSC's own reward | "Fair for co-op"; "nobody misses loot, nothing doubles" | Asad, 2026-10-24 |
| Difficulty | Vanilla co-op: only bosses get stronger, by the game's own Lua_MultiDoping (SpEffect 7500/7501: HP and status resistance x1.5 with one cooperator, x2.0 with two, poise +30/+60). BBSC's scaling is off (`SHADPS4_BBSC_SCALING=0`), which also brings that buff back (BBSC patches it out). No extra enemies. Replaces +50% HP and +10% damage on every enemy (2026-10-17) | "Not like the usual Bloodborne co-op rules, that should be fixed" | Asad, 2026-10-24 |
| Respec | In the Hunter's Dream ("Reset Attributes" in a co-op session too, Asad 2026-10-24; the weapon refund outside one). "Reset Attributes" (at the Doll) returns level and stats to the origin's and refunds every echo spent on levels, for 1 Insight (`BB_RESPEC_FREE=1`: free). "Refund Weapon Upgrades" (at the workbench, just before Leave; moved from the Doll 2026-10-12, Asad) opens the Fortify list of upgraded weapons, +10 and firearms included. Picking one sets it to +0 for free and returns every upgrade material, with fitted gems going to the inventory. The echoes spent on upgrades are lost. Saves a backup first. Built and verified in game 2026-10-10 (`src/runtime_respec.c`, `docs/bbcoop/respec_re.md`) | | Asad, 2026-10-10 |
| Respec entry | A new option in the Doll's dialogue (her talk script), not a settings-menu button | Feels native; chosen over the faster menu button | Asad, 2026-10-09 |
| Seamless engine | Upstream 0.6's BBSC (`BB_BBSC=1`, the online module) is the seamless co-op, run with vanilla co-op difficulty (BBSC's scaling off, 2026-10-24) and bbcoop's mods (respec, gun gems, camera, IK). bbcoop's own seamless patches and hooks (`BB_SEAMLESS=1`) stay as the fallback and are skipped while BBSC is on: both patch the same sites. BBSC already has scaling, guest items and boss rewards, shared progress, lamp warps together, up to five hunters, the host-death rule and respawn in place | "Take the best of both" | Asad, 2026-10-17 |
| Frame rate | 0.6's frame-rate hooks (48 per-frame values follow the frame time, running fixed), with bbcoop's camera chase rates and foot-IK correction on top (no shared sites). bbcoop's Uncap FPS++ patch stays selectable with `BB_FRAME_HOOKS=0` | "The best one, a mix of both" | Asad, 2026-10-17 |
| Seamless patches | Go through `src/runtime_seamless.c`, which checks the original bytes, not bbport's XML loader | The XML path writes without checking, so a wrong address would silently corrupt the game | Claude |
| Our code | In new `src/runtime_*.c` files, which `build.sh` already compiles, with only small marked edits to upstream files | bbport changes every day, so rebasing must stay cheap | Claude |
| US edition | CUSA00900 v1.09 is supported alongside CUSA03173 v1.09, with no per-build offset table | Its eboot.bin loads to the same image as EU 1.09 (SHA-256 071df19c…4c0a), so every offset and import is the same. Saves are per title ID, and the NP comm ID is region-specific (US NPWR05818_00) | Asad (request); checked 2026-10-09 by Claude |
| macOS | An x86-64 build of the same runtime, run by Rosetta 2, with MoltenVK for Vulkan; since 0.6 upstream's port (PR 191, bbport `macos-0.6`). Replaces the shelved static recompiler (branch `recomp`) | macOS 27 still ships Rosetta in full (Apple keeps a games subset after it), and it runs every Jaguar instruction the game uses (AVX, BMI1, MOVBE, LZCNT, F16C; checked 2026-10-12) when `ROSETTA_ADVERTISE_AVX=1`. Days of porting instead of months of lifter work, and the co-op code stays one form | Asad (request), Claude (route), 2026-10-12 |
| Windows | A native build (MSYS2 CLANG64) on upstream 0.6's Win32 layer, with Supermedo/bloodborne_pc's Windows build step, red-zone fix, mod links, launcher and package copied in; the online module (shadPS4 0.20, gen 20) linked into bb-probe.exe. `docs/bbcoop/windows.md` | Cross-play tests on Linux, macOS and Windows; friends get a zip with Bloodborne.exe | Asad, 2026-10-21 |
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

## Easy online (plan, 2026-10-25)

Asad's decisions (2026-10-24, final): the host's launcher runs the server (no rented server; Seamless Hunt
dropped as the default). Guests join over Tailscale (no UPnP on Asad's router). Accounts are made on first
start from a hunter name only. Friends get a prebuilt download: an unsigned Mac `.app` zip and the Linux
AppImage. Mac first, Linux second, Windows later.

Engineering calls (Claude):
- **One toolkit-free module, two windows.** `launcher/bbcoop_online.py` holds hosting, the peer list,
  accounts and the build check. The GTK launcher (Linux) and the Tk launcher (`bbport_launcher_win.py`,
  already frozen with PyInstaller on Windows) both call it. The Tk launcher becomes the Mac launcher too:
  GTK and libadwaita don't freeze well on macOS, and Windows gets the same page for free later.
- **Build check:** a build is its git commit (`git rev-parse`, or the `BUILD` file a package carries).
  The host's game server answers `GET :20443/bbcoop/hello` with its build and hunter name. Join
  refuses a host on another commit and names both commits.
- **Peers:** `tailscale status --json`, online peers whose TCP 31313 answers, named by their hello.
- **Accounts:** shadNet's own Create command (TCP 31313; the WebAPI has no account route), sent by the
  launcher before the game starts. One name and one generated password per install, created on each
  host the first time; "name taken" if another install already has it there. Open registration:
  only the tailnet reaches the server.
- **Host processes:** shadNet and `bbsc_gameserver.py` start when Host is picked and stop when it's
  unpicked or the launcher quits. The host's own game uses the same address its guests do.
- **Mac app:** PyInstaller's `.app` with the port in `Contents/Resources/port` (run.sh, scripts,
  `bin/`: bb-probe, libbbgpu, libbbnet20, MoltenVK, SDL3, Vulkan loader) and shadNet with its Qt
  (macdeployqt). Ad hoc signed. The frozen launcher runs the port's Python scripts (`--script`), so
  friends need no Python. Settings and data go in `~/Library/Application Support/bbcoop`.

Steps (each built and verified before the next):
0. Merge `windows` into `main`; the build check.
1. Online page: Off / Host / Join / Other server (the old manual fields). Host shows "Your address".
2. Join: hosts found on the tailnet, plus a manual address box; the three server fields follow it.
3. Accounts: first start asks for a hunter name; password generated; account made on join.
4. Mac `.app` zip, then the AppImage with the server and the respec tools.
5. `docs/bbcoop/friends.md` rewritten; Mac host / Linux join and the reverse; a fresh account.

Status (2026-10-25): steps 0-4 built. Verified on the Mac: hosting from the Tk launcher, the source
tree and the packaged app (shadNet and the game server on the address, an account from another
client, the build check, both stopped on close and after a killed launcher); the GTK page (run on
macOS with Homebrew's GTK 4); the packaged app's game start offline (respec mod built by the frozen
launcher, MoltenVK window, the game's code running). Not verified: the AppImage (needs Nix on Linux),
a co-op session through Host/Join (needs two machines and Tailscale), Gatekeeper's first open of a
downloaded zip, Windows.

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
  - At 30 FPS some frames showed 50 ms then 17 ms (judder): the 60 Hz vblank was a free-running timer.
    Fixed 2026-10-19 (0370330): present times and vblank phase follow the display (VK_GOOGLE_display_timing);
    frames off the 2-refresh grid 16.7% -> 4.8% at the Central Yharnam street, most of it one hitch.
  - 2026-10-19: Asad plays locked 30 at 720p with every effect on (but SSR) and model_lod 0: the street runs ~44 FPS
    uncapped there (900p: ~37, worst frames over 33 ms). 60 is out of reach in the streets (~55 with every effect
    off; GPU-bound, 1127 draws/frame). Lean texture usages (8ddcac0, Huax P10.1) +5 FPS; their mist flicker in
    1 of 2 sessions fixed by remembering promoted images. Immediate present had the steadiest pacing uncapped
    (p99 19.8 vs 23.1 ms FIFO); not made the default.
  - Asad still saw flicker at the Dream's graves during test runs (30 FPS, before the pacing fix); dumped frames
    were clean (whole-frame tile check), so it was on-screen pacing. Recheck with the fix.
  - The intermittent "PM4 type 0" crash on loads (2 of ~14 loads today) is a game race between the main
    thread's GPU setup (0xebb960) and GXRenderThread's frame job (0x2199af0) on one immediate context;
    Wooloomooloo2/bloodborne_mac_port 12a0ac5 serialises them with one lock; ported 2026-10-18
    (`src/runtime_context_lock.c`, `BB_CONTEXT_LOCK=0` off). It still happened in 4 of 84 test launches; since
    ad3c5ce a bad packet skips the rest of its buffer instead of ending the game. Installed and played at the Dream; loads with
    the lock not yet counted.
- **Upstream 0.6 merged (2026-10-19):** online on shadPS4 0.20 (`gpu/bbnet20`, `libbbnet20.so`, now the default;
  `BB_NET_GEN=19` = the older `gpu/bbnet`), The Hunter's Dream pool (Discord sign-in, without BBSC), Laurence above 30 FPS,
  flicker and load-crash fixes. bbcoop's BBSC changes below (loot, packets 0x23..0x27, host-lamp death, ss.info format,
  CUSA03173 serial, APP_VER 01.09, image reads) are ported into bbnet20 too; macOS stays on bbnet (bbnet20 has no Mac port).
  Built and the C/Python tests pass; not yet run in the game.
- **NPC helpers (Old Hunter Bell) in a session (2026-10-24):** only the host can summon one, as in vanilla
  (the game's summon builder refuses a guest), and only while fewer than 2 guests are in the world (the
  map events' own rule). The guest no longer gets the host's "summon spot available" flags, which showed
  it a spot that did nothing. A guest's follow reload keeps the phantom type (`coop_respawn_re.md`).
- **Mirrored loot on BBSC (2026-10-17, merged):** every pickup, enemy drop and boss drop goes to every hunter
  (`gpu/bbnet/shadps4/core/bbcoop_loot.inc`, relay "SHAD" 0x28, `SHADPS4_BBSC_MIRROR_LOOT=0` off,
  `docs/bbcoop/coop_loot_re.md`). Verified alone with its test seam (receive side); a real pickup's send side and a
  real session are not.
- **BBSC packets 0x23..0x27 (2026-10-17, fixed):** life, scaling, PvP, shared progress and the boss bar were sent
  but dropped by the receive thread (upstream 0.6-pre1 too), so a guest never got the host's scaling or
  progress. Now handed on. Needs a real session to see end to end.
- **Guest death at the host's lamp (2026-10-17):** BBSC respawns a guest at the host's lamp only when that
  lamp is loaded on the guest's side; bbcoop now loads the guest there otherwise (outside boss fights,
  `SHADPS4_BBSC_DEATH_TO_HOST_LAMP`, `docs/bbcoop/coop_respawn_re.md`). Verified alone with the test seam; the
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
  - **Camera:** the game's own turn ramp (0.6 s at full stick, 120 to 260°/s) is off, and the 30 FPS chase rates follow the frame time (`src/runtime_camera.c`, `docs/bbcoop/camera_re.md`).
  - **Messengers:** the notes' and bloodstains' messengers emerge within 5 m and sink beyond it, by design. Static analysis found no frame-rate, Intel-math or renderer cause (`docs/bbcoop/messengers_re.md`). `BB_MESSENGER_RADIUS` tests it in the game.
- **Firearm gem slots (Asad, 2026-10-11):**
  - Every firearm already fortifies to +10.
  - The Cannon, Church Cannon, Rosmarinus, Flamesprayer, Fist of Gratia and Hunter's Torch get the same single gun-gem slot from +1 that the pistols have, through `tools/respec_mod.py`. Its shape is Droplet, except Triangle on the Flamesprayer, Rosmarinus and Hunter's Torch and Radial on the Fist of Gratia (2026-10-17).
  - Shields and the Torch stay as they are.
- **Fire Paper, Bolt Paper, Empty Phantasm Shell (Asad, 2026-10-18), verified in game and accepted by Asad:**
  - Instead of a flat +80 per hit, the bonus is 80 × (base at +0 × the attack's multiplier) / 90 + 10% × (base at the current level × multiplier).
  - It keeps the early-game punch and adds a late-game share, and no longer favours fast weapons.
  - Viscerals still get nothing, and stats and gems don't change it.
  - `src/runtime_papers.c`, `docs/bbcoop/papers_re.md`; `BB_PAPERS=vanilla` off, `BB_PAPERS_TRACE=1` logs each bonus.
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
  - The AppImage now ships `tools/respec_mod.py` with soulstruct (2026-10-25, `packaging/default.nix`); not built yet.

- **Online in the real game (2026-10-09, US 1.09, local server):** NP sign-in, `ss.info` (`/bb-us/ss.info`), `/basic_utils/login` and `/basic_utils/sync_chara_id` (needs `PublishCharacterIdList` of `PublishCharaId`) are accepted. Next blocker: the `BloodMessSearchAdd` response (`/blood_messenger/exist_messages`) is rejected ("Failed to record note search information") even with `LostBloodMessIdList: []`; the parser takes the array (type 6) at image 0x1e84f12, so the cause is elsewhere (ResKind meaning?). Response keys are UTF-16 in the eboot at file offset 0x49d0d40. Reach it with the title, main menu Log In, then Continue (the title remembers Play Offline).

