#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
if [[ $(uname -s) == Darwin ]]; then
    # Rosetta 2 runs the x86-64 build; it executes AVX2 either way, this also reports it in CPUID.
    export ROSETTA_ADVERTISE_AVX=1
    # MoltenVK (Vulkan on Metal): BB_MOLTENVK_ICD names its MoltenVK_icd.json when the
    # Vulkan loader does not find it in a standard location.
    if [[ -z ${VK_DRIVER_FILES:-} && -n ${BB_MOLTENVK_ICD:-} ]]; then export VK_DRIVER_FILES=$BB_MOLTENVK_ICD; fi
    # bbcoop: else the MoltenVK scripts/mac-deps.sh builds (with the Mac port's changes: MetalFX).
    mvk_icd=${BB_MACOS_DEPS:-$PWD/out/mac-deps}/share/vulkan/icd.d/MoltenVK_icd.json
    if [[ -z ${VK_DRIVER_FILES:-} && -f $mvk_icd ]]; then export VK_DRIVER_FILES=$mvk_icd; fi
    # Metal argument buffers make every resource resident on each use; with the game's memory as
    # imported buffers of gigabytes that halved the frame rate (8 against 16.5 FPS, M4, clinic).
    export MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=${MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS:-0}
    # Fences written when the GPU has finished the work before them (the new memory model sets this
    # by itself; on macOS the GPU uses copies of the game's memory). Written at decode, they let the
    # game rewrite command buffers still queued: "PM4 type 0" in most sessions.
    export BB_HONEST_LABELS=${BB_HONEST_LABELS:-1}
    # Labels the GPU writes itself need VK_AMD_buffer_marker; without it each one ends the render
    # pass for a copy, and on a tile-based GPU every restart stores and reloads its targets.
    export BB_GPU_LABELS=${BB_GPU_LABELS:-0}
fi
if [[ ${1:-} == --software ]]; then
    shift
    if [[ -z ${VK_DRIVER_FILES:-} ]]; then
        for candidate in /run/opengl-driver/share/vulkan/icd.d/lvp_icd*.json /usr/share/vulkan/icd.d/lvp_icd*.json; do
            if [[ -f $candidate ]]; then export VK_DRIVER_FILES=$candidate; break; fi
        done
    fi
    if [[ -z ${VK_DRIVER_FILES:-} ]]; then echo 'Lavapipe not found; set VK_DRIVER_FILES.' >&2; exit 1; fi
    export VK_LOADER_LAYERS_DISABLE='~implicit~'
fi
# BB_PREBUILT=1 (packaged builds, the AppImage): out/bb-probe and its GPU library are installed
# next to this script; nothing is built and no nix-shell is needed.
# BB_DATA_DIR: writable directory for the generated files (out/), saves (user/) and bbport.ini;
# by default this directory.
data=${BB_DATA_DIR:-.}
out=$data/out
mkdir -p "$out"
export BB_CONFIG=${BB_CONFIG:-$data/bbport.ini}
# GPU paths are initialized when libbbgpu is loaded, before bbgpu_init receives --user.
# Select the same user directory now, including for an AppImage or a separate save profile.
export BB_GPU_USER_DIR=${BB_GPU_USER_DIR:-${BB_USER_DIR:-$data/user}}
# The drivers' own shader caches keep compiled pipelines between sessions (the port's cache gives
# them the shaders again at startup): NVIDIA's without its cleanup and with room for the game's
# pipelines (as Steam sets it), Mesa's with more room than its default.
export __GL_SHADER_DISK_CACHE=${__GL_SHADER_DISK_CACHE:-1}
export __GL_SHADER_DISK_CACHE_SKIP_CLEANUP=${__GL_SHADER_DISK_CACHE_SKIP_CLEANUP:-1}
export __GL_SHADER_DISK_CACHE_SIZE=${__GL_SHADER_DISK_CACHE_SIZE:-10737418240}
export MESA_SHADER_CACHE_MAX_SIZE=${MESA_SHADER_CACHE_MAX_SIZE:-4G}
mkdir -p "$BB_GPU_USER_DIR"
# FSR 4.1.1 assets (tools/fsr4cap/build_assets.sh): next to run.sh or in the data directory.
if [[ -z ${BB_FSR411_DIR:-} && ! -d fsr4_411 && -d $data/fsr4_411 ]]; then
    export BB_FSR411_DIR=$data/fsr4_411
fi
if [[ -z ${BB_PREBUILT:-} && -z ${BB_IN_NIX_SHELL:-} ]] && ! { command -v pkg-config >/dev/null && pkg-config --exists vulkan sdl3; } && command -v nix-shell >/dev/null; then
    args=''; if (( $# )); then args=$(printf '%q ' "$@"); fi
    exec env BB_IN_NIX_SHELL=1 nix-shell shell.nix --run "bash run.sh $args"
fi
# BB_SAVE_LOG=1 (launcher: "Save the log and statistics to a file"): this run's output and its
# per-frame statistics also go to $data/logs/<time>.log, .frames.csv and .readbacks.csv.
if [[ ${BB_SAVE_LOG:-} == 1 ]]; then
    logs=$data/logs
    mkdir -p "$logs"
    # printf's %(...)T needs bash 4.2 (macOS has 3.2): date there.
    printf -v stamp '%(%Y%m%d_%H%M%S)T' -1 2>/dev/null || stamp=$(date +%Y%m%d_%H%M%S)
    export BB_FRAME_STATS=1 BB_FRAME_LOG=${BB_FRAME_LOG:-$logs/$stamp.frames.csv}
    export BB_READBACK_LOG=${BB_READBACK_LOG:-$logs/$stamp.readbacks.csv}
    echo "Log: $logs/$stamp.log"
    # A descriptor bash picks (10 and up), not 3: the AppImage's runtime keeps its mount for as
    # long as the program holds an inherited pipe (the first free descriptor, 3); with --play
    # this script replaced the launcher, and closing that pipe unmounted the store under it
    # ("python3: Software caused connection abort").
    exec {log_fd}>"$logs/$stamp.log"
    # Bash builtins only (the AppImage's PATH has no tee).
    exec > >(while IFS= read -r line || [[ -n $line ]]; do printf '%s\n' "$line"; printf '%s\n' "$line" >&$log_fd; done) 2>&1
fi
if [[ -z ${PYTHON:-} ]]; then
    PYTHON=$(command -v python3 || true)
    if [[ -z $PYTHON ]]; then
        for candidate in /nix/store/*-python3-*/bin/python3; do
            if [[ -x $candidate ]]; then PYTHON=$candidate; break; fi
        done
    fi
fi
if [[ -z ${PYTHON:-} ]]; then echo 'Install Python 3 or set PYTHON.' >&2; exit 1; fi
# BB_GAME_DIR: the game's folder (eboot.bin, sce_module, ...); default next to this directory.
game=${BB_GAME_DIR:-../CUSA03173}
if [[ ! -f $game/eboot.bin ]]; then echo "No eboot.bin in $game (set BB_GAME_DIR)." >&2; exit 1; fi
original_game=$game
# bbcoop: the respec data mod is made from this game's own files (tools/respec_mod.py, soulstruct in
# BB_RE_PYTHON), again when the generator, the game folder or its two source files change.
respec_mod=${BB_MODS_DIR:-$data/mods}/bbcoop-respec
re_python=${BB_RE_PYTHON:-$HOME/.local/share/bbcoop-re/bin/python}
respec_talk=$game/dvdroot_ps4/script/talk/m21_00_00_00.talkesdbnd.dcx
stat_size_time=(-c '%s %Y'); [[ $(uname -s) == Darwin ]] && stat_size_time=(-f '%z %m') # bbcoop: BSD stat
if [[ -f $respec_talk ]] && command -v realpath cksum stat > /dev/null 2>&1; then
    respec_stamp="$(realpath -- "$game" 2>/dev/null || true) $(cksum < tools/respec_mod.py) $(stat "${stat_size_time[@]}" -- \
        "$respec_talk" "$game/dvdroot_ps4/msg/engus/menu.msgbnd.dcx" 2>/dev/null || true)"
    # bbcoop: gun gems (a gem slot on every firearm, in its gameparam; BB_GUN_GEMS=0: none) rebuild it too.
    respec_stamp+=" gun_gems=$([[ ${BB_GUN_GEMS:-1} == 0 ]] && echo 0 || echo 1) $(stat "${stat_size_time[@]}" -- \
        "$game/dvdroot_ps4/param/gameparam/gameparam.parambnd.dcx" "$game/dvdroot_ps4/msg/engus/item.msgbnd.dcx" 2>/dev/null || true)"
    if [[ ! -d $respec_mod/dvdroot_ps4 || $(cat -- "$respec_mod/.stamp" 2>/dev/null) != "$respec_stamp" ]]; then
        if [[ -x $re_python ]] && "$re_python" -I tools/respec_mod.py "$game" "$respec_mod.new" > "$out/respec_mod.log" 2>&1; then
            printf '%s' "$respec_stamp" > "$respec_mod.new/.stamp"
            rm -rf -- "$respec_mod"; mv -- "$respec_mod.new" "$respec_mod"; echo "Respec mod: built $respec_mod"
        else
            rm -rf -- "$respec_mod.new"; echo "Respec mod: not built (needs soulstruct in $re_python; see $out/respec_mod.log)" >&2
            # bbcoop: an older build's gun gems must not outlive BB_GUN_GEMS=0.
            [[ ${BB_GUN_GEMS:-1} == 0 ]] && rm -f -- "$respec_mod/dvdroot_ps4/param/gameparam/gameparam.parambnd.dcx"
        fi
    fi
fi
game=$("$PYTHON" scripts/mods.py "$game" --out "$out" \
    --mods-dir "${BB_MODS_DIR:-$data/mods}" --config "${BB_MODS_CONFIG:-$data/mods.json}" \
    --enabled "${BB_MODS_ENABLED:-1}")
# A private merged view lasts for this launch, including restarts. Cleanup only our own view.
if [[ $game != "$(realpath "$original_game")" ]]; then
    mod_game=$game
    trap '"$PYTHON" -c '\''import shutil,sys; shutil.rmtree(sys.argv[1])'\'' "$mod_game"' EXIT
fi
"$PYTHON" scripts/prepare.py "$game" --out "$out"
"$PYTHON" scripts/link_libc.py "$game" --out "$out"
"$PYTHON" scripts/link_modules.py "$game" --out "$out"
"$PYTHON" scripts/content_profile.py "$game" --out "$out" --sku "${BB_CONTENT_SKU:-full}"
# Sizes chosen below for the previous launch are recomputed after an in-game restart.
if [[ ${BB_AUTO_RENDER_RES:-} == 1 ]]; then
    unset BB_RENDER_RES BB_OUTPUT_RES BB_AUTO_RENDER_RES
fi
# BB_RENDER_RES=WxH explicitly sets the game's render resolution (a patch at start).
# Frame rate: BB_FPS=uncap (default; hooks in the game's code, gpu/shim/bbport_frame_rate.cpp:
# each frame's real time; vblank 480 Hz, frames shown at once; BB_FRAME_HOOKS=0: the patches),
# 60/90 (the same hooks, at most 60/90 frames a second) or 30 (the game's own). BB_PATCHES adds
# patch names ("a;b").
# macOS: 30 by default (an M1 Pro's GPU needs ~28 ms a frame at 1080p; faster rates came out uneven).
if [[ $(uname -s) == Darwin ]]; then fps=${BB_FPS:-30}; else fps=${BB_FPS:-uncap}; fi
# The GPU library's frame rate hooks read the mode from the environment (unset: uncap).
export BB_FPS=$fps
# bbcoop: BB_FPS=120 is the hooks at most 120 frames a second (vblank 120). With BB_FRAME_HOOKS=0 the
# patches: 120 FPS++ for 120, ps4_cheats' Uncap FPS++ (2026-10-10) for uncap, and BB_FPS_PATCH=old the
# previous Uncap FPS++ (2026-08-28) with the separate sprint fix (scripts/patches.py reads it).
# bbcoop A/B for those patches: BB_FPS_SKIP=<group>[,..] leaves groups of Uncap FPS++ lines out (cloth grass sfx
# fps60 hknp timeobj clip), BB_FPS_TRY=<variant>[,..] picks bbcoop fixes (step1972 wind30 wind60 clothstep dtfloor; default wind60,clothstep,dtfloor,
# none for none); docs/bbcoop/animation_re.md.
# bbport.ini output_res other than 1080p (720p for the Steam Deck, 1440p, 2160p): the whole game
# renders at the preset's size of the output (a patch), the upscaler fills the output, the UI is
# drawn at the output size. Preset and output changes need a restart.
# Live resolution changes keep the guest at 1920x1080 and scale host targets at run time instead
# (output and presets change in the menu without a restart, but post-processing stays at 1080p
# and scene targets are copied back: much slower on the Steam Deck and older GPUs). Chosen by
# BB_LIVE_RES=0/1, else bbport.ini live_resolution=0/1/auto (auto: the GPU check, strong
# discrete GPUs get them); off when unset. 1080p output and TAA always use the live path.
# The GPU tools (bb-gpu-capabilities: the game's GPU, its live resolution support).
if [[ -n ${BB_PROBE:-} ]]; then caps=$(dirname -- "$BB_PROBE")/bb-gpu-capabilities
elif [[ -n ${BB_PREBUILT:-} ]]; then caps=bin/bb-gpu-capabilities
else caps=out/bb-gpu-capabilities; fi
if [[ -z ${BB_RENDER_RES:-} ]]; then
    read -r scaled_render scaled_output < <("$PYTHON" scripts/patches.py --print-scaled --settings "$BB_CONFIG") || true
fi
live=0
if [[ -n ${scaled_output:-} ]]; then
    live=${BB_LIVE_RES:-}
    # Bash builtins only: the AppImage's PATH has no sed/grep (a missing one ended run.sh silently).
    if [[ -z $live && -f $BB_CONFIG ]]; then
        while IFS= read -r line || [[ -n $line ]]; do
            [[ $line =~ ^live_resolution=([01]|auto)$ ]] && live=${BASH_REMATCH[1]}
        done < "$BB_CONFIG"
    fi
    if [[ $live == auto ]]; then
        live=$("$caps" --live-resolution 2> >(while IFS= read -r line; do
            [[ $line == *MANGOHUD* ]] || printf '%s\n' "$line"; done >&2)) || live=0
    fi
    [[ $live == 1 ]] || live=0
fi
if [[ $live == 1 ]]; then
    echo "Output ${scaled_output}: live resolution changes (live_resolution=0: startup patch)"
elif [[ -n ${scaled_output:-} ]]; then
    export BB_RENDER_RES=$scaled_render BB_OUTPUT_RES=$scaled_output BB_AUTO_RENDER_RES=1
    export BB_DMEM_MB=${BB_DMEM_MB:-9152}
    echo "Output ${scaled_output}: scene ${scaled_render}, direct memory ${BB_DMEM_MB} MiB (live_resolution=1: live changes)"
fi
# bbcoop: online play needs the title's Play Online, which the default patch skips (offline).
[[ ${BB_ONLINE:-0} == 1 ]] && export BB_SKIP_NETWORK_CHOICE=${BB_SKIP_NETWORK_CHOICE:-0}
"$PYTHON" scripts/patches.py --out "$out" --fps "$fps" --extra "${BB_PATCHES:-}" --settings "$BB_CONFIG" --game-dir "$game" --render-res "${BB_RENDER_RES:-}" --output-res "${BB_OUTPUT_RES:-}" \
    --patches-dir "${BB_PATCHES_DIR:-$data/patches}" --patches-config "${BB_PATCHES_CONFIG:-$data/patches.json}"
# Background upload of the game's GPU memory into VRAM ahead of use (BufferCache::Preupload):
# 1 = only memory already in VRAM that the game rewrote (no new VRAM), 2 = all of it (~3 GB more
# VRAM), 0 = off. BB_GUEST_GPU_MEMORY=1 puts guest direct memory in GPU-visible dma-buf chunks
# (BB_GUEST_IN_PLACE below implies it: it commits all allocated memory up front and rules out BB_UFFD).
export BB_PREUPLOAD=${BB_PREUPLOAD:-1}
# Memory model and translation. BB_PC_MODEL=1 (the launcher's "Memory model": "Auto" gives it to
# every GPU; BB_PC_MODEL=1/0 by hand) selects how a PC release would
# work: the GPU uses the game's memory where it is (GPU-visible system memory) and keeps the data
# it reads often in VRAM, the command processor's work is translated rather than emulated
# (BB_GUEST_IN_PLACE=1 and what depends on it). Every GPU gets the layer's memory module, which
# binds the game's memory in place and keeps VRAM copies without sparse binding (NVIDIA's sparse
# binding stalls the GPU for seconds; on AMD it is faster than the sparse arena too);
# BB_LAYER_MEMORY=0: AMD's sparse arena, as before 0.52.
# 0: the model of 0.3 (VRAM copies of the game's memory, write tracking),
# with the fixes made since. BB_GUEST_IN_PLACE set by hand overrides it.
# BB_AS_0_3=1 (the launcher's developer switch "Synchronisation as in 0.3"): what changed since the
# 0.3 release is reverted for comparisons: the 0.3 memory model, WRITE_DATA/DMA waiting for every
# host copy, the scheduler's concurrent recording check.
# The game's GPU (vendor ID and name), for the choices below that depend on it.
gpu_vendor= gpu_name=
if [[ -z ${BB_PC_MODEL:-} || ( ${BB_AUTO_NOHIZ:-1} != 0 && -z ${RADV_DEBUG:-} ) ]]; then
    IFS=$'\t' read -r gpu_vendor gpu_name < <("$caps" --device 2> /dev/null) || true
fi
# Memory model left to the port: the new one on every GPU (0.6). It goes through the layer's memory
# module: no sparse binding of the game's memory, so nothing NVIDIA-specific stalls; a driver that
# fails the startup checks (BbGuestMemory::Usable) still falls back to VRAM copies by itself.
if [[ -z ${BB_PC_MODEL:-} ]]; then
    export BB_PC_MODEL=1
    echo "Memory model: new (${gpu_name:-GPU unknown}; BB_PC_MODEL=0: the 0.3 model)"
fi
# GCN 3-5 under RADV (Polaris, Vega and their APUs): with HiZ, depth compression showed black
# halos and missing geometry (issues #54, #99); RADV_DEBUG=nohiz fixed it at some cost. Set
# unless RADV_DEBUG is the user's, BB_AUTO_NOHIZ=0: off.
if [[ ${BB_AUTO_NOHIZ:-1} != 0 && -z ${RADV_DEBUG:-} && $gpu_name == *"(RADV "* ]]; then
    for chip in TONGA ICELAND CARRIZO FIJI STONEY POLARIS VEGA RAVEN RENOIR; do
        if [[ $gpu_name == *"(RADV $chip"* ]]; then
            export RADV_DEBUG=nohiz
            echo "RADV: $gpu_name: RADV_DEBUG=nohiz (depth compression artifacts on GCN 3-5; BB_AUTO_NOHIZ=0: off)"
            break
        fi
    done
fi
if [[ ${BB_AS_0_3:-0} == 1 ]]; then
    export BB_GUEST_IN_PLACE=0 BB_HOST_COPY_WAITS=all BB_PRODUCER_CHECK=1
fi
export BB_GUEST_IN_PLACE=${BB_GUEST_IN_PLACE:-${BB_PC_MODEL:-0}}
# MangoHud (launcher switch: MANGOHUD=1) must be drawn once. Two overlays on top of each other
# showed doubled, offset text: the Steam Deck's performance overlay (mangoapp, game mode) plus the
# in-game layer, or the AppImage's bundled layer plus a system MangoHud (the layer names differ,
# so the Vulkan loader loads both).
if [[ ${MANGOHUD:-0} == 1 ]]; then
    if pgrep -x mangoapp > /dev/null 2>&1; then
        echo "MangoHud: Steam's performance overlay is on; the in-game MangoHud stays off"
        unset MANGOHUD
        export DISABLE_MANGOHUD=1
    elif grep -qs '"VK_LAYER_MANGOHUD' /usr/share/vulkan/implicit_layer.d/*.json \
            /etc/vulkan/implicit_layer.d/*.json \
            "${XDG_DATA_HOME:-$HOME/.local/share}"/vulkan/implicit_layer.d/*.json; then
        # The system's own MangoHud (and its config) is used; the bundled one is skipped.
        export VK_LOADER_LAYERS_DISABLE=${VK_LOADER_LAYERS_DISABLE:+$VK_LOADER_LAYERS_DISABLE,}VK_LAYER_MANGOHUD_overlay_64_x86_64
    fi
fi
# Write tracking with userfaultfd write-protection instead of mprotect (no address-space write lock:
# a streaming burst re-protected thousands of pages, ~10-20 ms); falls back to mprotect when the
# kernel lacks it. Off by default for now: the Steam Deck crashed 4 times in 9 minutes with it
# (2026-10-04, PM4 type 0 = zeroed command buffers, heap corruption); 1 turns it on.
export BB_UFFD=${BB_UFFD:-0}
# The command buffers of each submission are copied when the game submits them and decoded from
# the copy: while an area loads the game reused that memory before the GPU thread got to it
# ("Unimplemented PM4 type 0" a few seconds after loading a save). 0 decodes guest memory.
export BB_COPY_GPU_BUFFERS=${BB_COPY_GPU_BUFFERS:-1}
# A guest write next to small GPU outputs the GPU is still writing (counters, compute results,
# up to BB_GPU_WRITE_TWINS_MAX bytes per page) does not wait for the GPU (BufferCache twins).
export BB_GPU_WRITE_TWINS=${BB_GPU_WRITE_TWINS:-1} BB_GPU_WRITE_TWINS_MAX=${BB_GPU_WRITE_TWINS_MAX:-65536}
if [[ -z ${BB_VBLANK_HZ:-} ]]; then
    case $fps in uncap) export BB_VBLANK_HZ=1000 ;; 90) export BB_VBLANK_HZ=90 ;; 120) export BB_VBLANK_HZ=120 ;; *) export BB_VBLANK_HZ=60 ;; esac # bbcoop: 120
fi
# bbcoop: uncapped frames are limited to 120 FPS unless BB_FPS_LIMIT is set (0 = no limit). The
# camera sped up at times while uncapped (Asad, 2026-10-10); src/runtime_camera.c has the fix.
if [[ $fps == uncap ]]; then export BB_FPS_LIMIT=${BB_FPS_LIMIT-120}; fi
# bbcoop: the fixed-frame-rate patches tune the camera's chase rates themselves (runtime_camera.c).
# Since 0.6 the 60/90/120 modes are the frame rate hooks, not patches, unless BB_FRAME_HOOKS=0.
if [[ $fps != uncap && ( $fps == 30 || ${BB_FRAME_HOOKS:-1} == 0 ) ]]; then export BB_CAMERA_RATES=${BB_CAMERA_RATES-0}; fi
# FSR 4: faster post passes next to the downloaded ones (incremental; tools/fsr4_optimize.sh).
if [[ -z ${BB_PREBUILT:-} && -d fsr4_shaders ]] && command -v spirv-cross >/dev/null; then
    bash tools/fsr4_optimize.sh || echo 'FSR 4: optimized post passes not built' >&2
fi
if [[ -n ${BB_PREBUILT:-} ]]; then
    probe=${BB_PROBE:-bin/bb-probe}
else
    bash build.sh
    probe=${BB_PROBE:-out/bb-probe}  # BB_PROBE: a wrapper (gdb) around it
fi
# Online play (BB_ONLINE=1, launcher: Online; the online module gpu/bbnet): the game's own server
# goes to the community server, summon signs to the shadNet WebAPI (scripts/online.py).
if [[ ${BB_ONLINE:-0} == 1 && -f scripts/online.py ]]; then
    export BB_SHADNET_SERVER=${BB_SHADNET_SERVER:-play.seamlesshunt.com:31313}
    # HTTPS needs CA certificates: OpenSSL from Nix looks only in its own store path (the
    # packaged wrapper brings a bundle; from source the system's is taken).
    if [[ ! -r ${SSL_CERT_FILE:-} ]]; then
        for bundle in "${NIX_SSL_CERT_FILE:-}" /etc/ssl/certs/ca-certificates.crt /etc/pki/tls/certs/ca-bundle.crt /etc/ssl/cert.pem /etc/ssl/ca-bundle.pem; do
            [[ -n $bundle && -r $bundle ]] && { export SSL_CERT_FILE=$bundle; break; }
        done
    fi
    [[ -n ${BB_SHADNET_WEBAPI:-} ]] || export BB_SHADNET_WEBAPI=http://${BB_SHADNET_SERVER%:*}:31315
    # BBSC: its game server on this PC when the server's own does not answer (online.py
    # bbsc-local). It lives as long as the game: a watcher stops it when this process (exec'd into
    # the game below) is gone. Messages and bloodstains are then this PC's only.
    # bbcoop: BBSC's co-op scaling with Asad's numbers (docs/bbcoop/brief.md: +50% HP and +10% damage per
    # extra player; bosses scale through HP too); BBSC's own stagger numbers. Each is overridable.
    if [[ ${BB_BBSC:-0} == 1 ]]; then
        export BBSC_SCALE_ENEMY_HP=${BBSC_SCALE_ENEMY_HP:-50} BBSC_SCALE_BOSS_HP=${BBSC_SCALE_BOSS_HP:-50}
        export BBSC_SCALE_DAMAGE=${BBSC_SCALE_DAMAGE:-10}
    fi
    if [[ ${BB_BBSC:-0} == 1 && -f scripts/bbsc_gameserver/bbsc_gameserver.py ]]; then
        bbsc_shadnet=$("$PYTHON" scripts/online.py bbsc-local) || bbsc_shadnet=
        if [[ -n $bbsc_shadnet ]]; then
            bbsc_dir=${BB_USER_DIR:-$data/user}/bbsc
            mkdir -p "$bbsc_dir/logs"
            game_pid=$$
            if ! (exec 3<>/dev/tcp/127.0.0.1/20443) 2>/dev/null; then
                ( "$PYTHON" scripts/bbsc_gameserver/bbsc_gameserver.py --shadnet "$bbsc_shadnet" \
                      --host 127.0.0.1 --port 20443 --relay-port 0 --no-bbhost --no-playlog \
                      --data "$bbsc_dir/online-data.json" --log-dir "$bbsc_dir/logs" \
                      > "$bbsc_dir/logs/gameserver-console.log" 2>&1 &
                  server_pid=$!
                  while kill -0 "$game_pid" 2>/dev/null && kill -0 "$server_pid" 2>/dev/null; do sleep 2; done
                  kill "$server_pid" 2>/dev/null ) &
                echo "Co-op: BBSC game server on 127.0.0.1:20443 (summons -> $bbsc_shadnet:31315)"
            else
                echo "Co-op: a game server answers on 127.0.0.1:20443 already: used"
            fi
            export BB_COMMUNITY_SERVER=http://127.0.0.1
        fi
    fi
    SHADPS4_HTTP_HOST_OVERRIDES_JSON=$("$PYTHON" scripts/online.py overrides --user "${BB_USER_DIR:-$data/user}") || true
    export SHADPS4_HTTP_HOST_OVERRIDES_JSON
fi
probe_args=("$out/boot-linked.bin" --content-profile "$out/content.bin" --patches "$out/patches.bin" --app0 "$game" --user "${BB_USER_DIR:-$data/user}" --timeout "${BB_TIMEOUT:-0}" "$@")
if [[ -n ${mod_game:-} ]]; then
    "$probe" "${probe_args[@]}" &
    mod_pid=$!
    trap 'kill -TERM "$mod_pid" 2>/dev/null || true' TERM INT
    mod_status=0
    wait "$mod_pid" || mod_status=$?
    # An interrupted wait must finish the child before removing its mounted view.
    if kill -0 "$mod_pid" 2>/dev/null; then wait "$mod_pid" || mod_status=$?; fi
    exit "$mod_status"
fi
if [[ $(uname -s) == Darwin ]]; then
    # macOS lets no app take the focus by itself: started from a terminal, the game's window
    # opened behind it and the keys went to the terminal. System Events brings this process
    # (bb-probe after the exec below keeps the PID) to the front once its window is up.
    (
        for _ in $(seq 30); do
            sleep 1
            front=$(osascript -e "tell application \"System Events\" to set frontmost of (first process whose unix id is $$) to true" \
                -e 'tell application "System Events" to get unix id of first process whose frontmost is true' 2>/dev/null) || continue
            [[ $front == "$$" ]] && break
        done
    ) </dev/null >/dev/null 2>&1 &
fi
exec "$probe" "${probe_args[@]}"
