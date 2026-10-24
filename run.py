#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""bbcoop on Windows: the counterpart of run.sh. Prepares the game image and starts bb-probe.exe.

The same environment variables as run.sh (BB_GAME_DIR, BB_DATA_DIR, BB_FPS, BB_ONLINE, BB_BBSC,
BB_USER_DIR, BB_MODS_*, ...); extra arguments go to bb-probe. Its Windows parts (running the
scripts inside the packaged Bloodborne.exe, the MSYS2 DLLs, no console windows) are from
Supermedo/bloodborne_pc's run.py. Not carried over from run.sh: what is Linux- or Mac-only
(Nix, Lavapipe, MangoHud, RADV, userfaultfd, MoltenVK, the mist skip), BB_SAVE_LOG (the launcher
writes user/last_run.log) and tools/fsr4_optimize.sh (bash and spirv-cross).
"""
import ctypes
import hashlib
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import threading

PORT = Path(__file__).resolve().parent
FROZEN = getattr(sys, 'frozen', False)


def fail(message):
    print(message, file=sys.stderr)
    sys.exit(1)


def no_console():
    """Started without a console (the launcher): console programs must not open their own."""
    return 0 if ctypes.windll.kernel32.GetConsoleWindow() else subprocess.CREATE_NO_WINDOW


def python_command(script, python=None):
    """A script run by Python: inside the packaged Bloodborne.exe (PyInstaller) the executable
    runs scripts itself (--script)."""
    if FROZEN:
        return [sys.executable, '--script', str(script)]
    return [python or sys.executable, str(script)]


def run_script(name, *args, capture=False):
    """scripts/<name>; a failure stops the launch. capture: returns its output."""
    command = python_command(PORT / 'scripts' / name) + [str(a) for a in args]
    # stdin given: the output handles are passed explicitly (a windowed Bloodborne.exe child would
    # get none otherwise).
    result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE if capture else None,
                            text=capture, creationflags=no_console())
    if result.returncode:
        sys.exit(result.returncode)
    return result.stdout if capture else None


def ini_value(path, key):
    try:
        for line in Path(path).read_text(encoding='utf-8', errors='replace').splitlines():
            match = re.fullmatch(rf'{re.escape(key)}=(.*)', line.strip())
            if match:
                return match.group(1)
    except OSError:
        pass
    return None


def tool_output(command):
    """A helper program's output, '' when it fails or is missing (run.sh: || true)."""
    try:
        result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL, text=True, creationflags=no_console())
    except OSError:
        return ''
    return result.stdout.strip() if result.returncode == 0 else ''


def find_executable(name):
    """bin/ (packaged), else out/ (built with build.sh)."""
    for candidate in (PORT / 'bin' / name, PORT / 'out' / name):
        if candidate.is_file():
            return candidate
    return None


def file_stamp(*paths):
    parts = []
    for path in paths:
        try:
            info = path.stat()
            parts.append(f'{info.st_size} {int(info.st_mtime)}')
        except OSError:
            parts.append('-')
    return ' '.join(parts)


def respec_mod(env, game, data, out):
    """bbcoop: the respec data mod, made from this game's own files (tools/respec_mod.py, soulstruct),
    again when the generator, the game folder or its source files change (run.sh)."""
    mod = Path(env.get('BB_MODS_DIR', data / 'mods')) / 'bbcoop-respec'
    dvdroot = game / 'dvdroot_ps4'
    talk = dvdroot / 'script/talk/m21_00_00_00.talkesdbnd.dcx'
    if not talk.is_file():
        return
    generator = PORT / 'tools' / 'respec_mod.py'
    gun_gems = '0' if env.get('BB_GUN_GEMS', '1') == '0' else '1'
    stamp = ' '.join((str(game.resolve()), hashlib.sha256(generator.read_bytes()).hexdigest(),
                      file_stamp(talk, dvdroot / 'msg/engus/menu.msgbnd.dcx'), f'gun_gems={gun_gems}',
                      file_stamp(dvdroot / 'param/gameparam/gameparam.parambnd.dcx',
                                 dvdroot / 'msg/engus/item.msgbnd.dcx')))
    try:
        built = (mod / '.stamp').read_text(encoding='utf-8')
    except OSError:
        built = None
    if (mod / 'dvdroot_ps4').is_dir() and built == stamp:
        return
    new = mod.with_name(mod.name + '.new')
    shutil.rmtree(new, ignore_errors=True)
    log = out / 'respec_mod.log'
    # Packaged: soulstruct is inside Bloodborne.exe. From source: BB_RE_PYTHON, else this Python.
    command = python_command(generator, env.get('BB_RE_PYTHON')) + [str(game), str(new)]
    with open(log, 'w', encoding='utf-8') as output:
        result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT,
                                creationflags=no_console())
    if result.returncode == 0:
        (new / '.stamp').write_text(stamp, encoding='utf-8')
        shutil.rmtree(mod, ignore_errors=True)
        new.rename(mod)
        print(f'Respec mod: built {mod}')
    else:
        shutil.rmtree(new, ignore_errors=True)
        print(f'Respec mod: not built (needs soulstruct; see {log})', file=sys.stderr)
        # bbcoop: an older build's gun gems must not outlive BB_GUN_GEMS=0.
        if gun_gems == '0':
            (mod / 'dvdroot_ps4/param/gameparam/gameparam.parambnd.dcx').unlink(missing_ok=True)


def bbsc_game_server(env, user_dir, probe_done):
    """BBSC: its game server on this PC when the server's own does not answer (online.py
    bbsc-local), for as long as the game runs (run.sh)."""
    server = PORT / 'scripts' / 'bbsc_gameserver' / 'bbsc_gameserver.py'
    if not server.is_file():
        return
    sys.path.insert(0, str(PORT / 'scripts'))
    import online
    shadnet = online.bbsc_local_server()
    if not shadnet:
        return
    try:
        socket.create_connection(('127.0.0.1', 20443), timeout=1).close()
        print('Co-op: a game server answers on 127.0.0.1:20443 already: used')
    except OSError:
        bbsc_dir = user_dir / 'bbsc'
        (bbsc_dir / 'logs').mkdir(parents=True, exist_ok=True)
        console = open(bbsc_dir / 'logs' / 'gameserver-console.log', 'w', encoding='utf-8')
        process = subprocess.Popen(
            python_command(server) + ['--shadnet', shadnet, '--host', '127.0.0.1', '--port', '20443',
                                      '--relay-port', '0', '--no-bbhost', '--no-playlog',
                                      '--data', str(bbsc_dir / 'online-data.json'),
                                      '--log-dir', str(bbsc_dir / 'logs')],
            stdin=subprocess.DEVNULL, stdout=console, stderr=subprocess.STDOUT, creationflags=no_console())

        def stop():
            probe_done.wait()
            process.terminate()
            console.close()
        threading.Thread(target=stop, daemon=True).start()
        print(f'Co-op: BBSC game server on 127.0.0.1:20443 (summons -> {shadnet}:31315)')
    env['BB_COMMUNITY_SERVER'] = 'http://127.0.0.1'


def online_setup(env, user_dir, probe_done):
    """Online play (BB_ONLINE=1): the shadPS4 0.20 module, linked into bb-probe.exe, answers both
    pools: Seamless Hunt (shadNet with BBSC) and The Hunter's Dream (BB_NET_BACKEND=thd, no BBSC)."""
    sys.path.insert(0, str(PORT / 'scripts'))
    import online
    env['BB_NET_GEN'] = '20'
    if env.get('BB_NET_BACKEND') == 'thd':
        env.pop('BB_BBSC', None)
    env.setdefault('BB_SHADNET_SERVER', online.SHADNET_SERVER)
    if not env.get('BB_SHADNET_WEBAPI'):
        env['BB_SHADNET_WEBAPI'] = 'http://' + env['BB_SHADNET_SERVER'].rsplit(':', 1)[0] + ':31315'
    if env.get('BB_BBSC') == '1':
        # bbcoop: vanilla co-op difficulty (docs/bbcoop/brief.md, Asad 2026-10-24): BBSC's scaling off,
        # which also lifts its patch-out of the game's own boss-only buff (Lua_MultiDoping).
        env.setdefault('SHADPS4_BBSC_SCALING', '0')
        bbsc_game_server(env, user_dir, probe_done)
    if env.get('BB_NET_BACKEND') == 'thd':
        # The Hunter's Dream (bbhost's protocol): the account comes from the launcher's Discord sign-in.
        if not env.get('BB_THD_TOKEN') or not env.get('BB_THD_ONLINE_ID'):
            fail("The Hunter's Dream: not signed in (launcher: Online co-op -> Sign in with Discord).")
        path, base = online.write_thd_overrides(user_dir, env.get('BB_COMMUNITY_SERVER'))
        env['SHADPS4_HTTP_HOST_OVERRIDES_JSON'] = str(path)
        env.setdefault('BB_THD_SERVER', f'{base}:{online.COMMUNITY_API_PORT}')
        env.setdefault('BB_THD_AUTH', base)
        for key in ('BB_SHADNET_SERVER', 'BB_SHADNET_WEBAPI', 'BB_SHADNET_NPID', 'BB_SHADNET_PASSWORD',
                    'BB_SHADNET_TOKEN'):
            env.pop(key, None)
    else:
        community = online.community_address(env.get('BB_COMMUNITY_SERVER'))
        webapi = online.webapi_setting(env.get('BB_SHADNET_WEBAPI'), env['BB_SHADNET_SERVER'], community)
        env['SHADPS4_HTTP_HOST_OVERRIDES_JSON'] = str(online.write_overrides(user_dir, community, webapi))


def main():
    args = sys.argv[1:]
    # As run.sh: relative paths (BB_GAME_DIR, BB_DATA_DIR, fsr4_shaders/) are from the port.
    os.chdir(PORT)
    env = os.environ
    data = Path(env.get('BB_DATA_DIR', PORT))
    out = data / 'out'
    out.mkdir(parents=True, exist_ok=True)
    env.setdefault('BB_CONFIG', str(data / 'bbport.ini'))
    config = env['BB_CONFIG']
    # GPU paths are initialized when the GPU library starts, before it receives --user.
    user_dir = Path(env.get('BB_USER_DIR', data / 'user')).resolve()
    user_dir.mkdir(parents=True, exist_ok=True)
    env.setdefault('BB_GPU_USER_DIR', str(user_dir))
    if not env.get('BB_FSR411_DIR') and not (PORT / 'fsr4_411').is_dir() and (data / 'fsr4_411').is_dir():
        env['BB_FSR411_DIR'] = str(data / 'fsr4_411')

    probe = Path(env['BB_PROBE']) if env.get('BB_PROBE') else find_executable('bb-probe.exe')
    if not probe or not probe.is_file():
        fail('bb-probe.exe not found: build it with build.sh (MSYS2 CLANG64) or use a packaged build.')
    caps = probe.parent / 'bb-gpu-capabilities.exe'
    # Development builds run from the MSYS2 tree: its CLANG64 DLLs (SDL3, FFmpeg, ...).
    if not (probe.parent / 'SDL3.dll').is_file():
        clang64 = Path(env.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64' / 'bin'
        if clang64.is_dir():
            env['PATH'] = f'{clang64}{os.pathsep}{env.get("PATH", "")}'

    # BB_GAME_DIR: the game's folder (eboot.bin, sce_module, ...); default next to this directory.
    game = Path(env.get('BB_GAME_DIR', PORT.parent / 'CUSA03173'))
    if not (game / 'eboot.bin').is_file():
        fail(f'No eboot.bin in {game} (set BB_GAME_DIR).')
    original_game = game.resolve()
    respec_mod(env, game, data, out)
    view = run_script('mods.py', game, '--out', out, '--mods-dir', env.get('BB_MODS_DIR', data / 'mods'),
                      '--config', env.get('BB_MODS_CONFIG', data / 'mods.json'),
                      '--enabled', env.get('BB_MODS_ENABLED', '1'), capture=True)
    game = Path(view.strip())
    # A private merged view lasts for this launch. Cleanup only our own view.
    mod_view = game if game.resolve() != original_game else None
    probe_done = threading.Event()
    try:
        run_script('prepare.py', game, '--out', out)
        run_script('link_libc.py', game, '--out', out)
        run_script('link_modules.py', game, '--out', out)
        run_script('content_profile.py', game, '--out', out, '--sku', env.get('BB_CONTENT_SKU', 'full'))
        # Sizes chosen below for the previous launch are recomputed after an in-game restart.
        if env.get('BB_AUTO_RENDER_RES') == '1':
            for key in ('BB_RENDER_RES', 'BB_OUTPUT_RES', 'BB_AUTO_RENDER_RES'):
                env.pop(key, None)
        # Frame rate (run.sh): uncap by default, read by the GPU library's frame rate hooks too.
        fps = env.get('BB_FPS', 'uncap')
        env['BB_FPS'] = fps
        scaled_render = scaled_output = None
        if not env.get('BB_RENDER_RES'):
            printed = run_script('patches.py', '--print-scaled', '--settings', config, capture=True).split()
            if len(printed) == 2:
                scaled_render, scaled_output = printed
        live = '0'
        if scaled_output:
            live = env.get('BB_LIVE_RES') or ini_value(config, 'live_resolution') or '0'
            if live == 'auto':
                live = tool_output([str(caps), '--live-resolution']) or '0'
            live = '1' if live == '1' else '0'
        if live == '1':
            print(f'Output {scaled_output}: live resolution changes (live_resolution=0: startup patch)')
        elif scaled_output:
            env.update(BB_RENDER_RES=scaled_render, BB_OUTPUT_RES=scaled_output, BB_AUTO_RENDER_RES='1')
            env.setdefault('BB_DMEM_MB', '9152')
            print(f'Output {scaled_output}: scene {scaled_render}, direct memory {env["BB_DMEM_MB"]} MiB '
                  '(live_resolution=1: live changes)')
        # bbcoop: online play needs the title's Play Online, which the default patch skips (offline).
        if env.get('BB_ONLINE') == '1':
            env.setdefault('BB_SKIP_NETWORK_CHOICE', '0')
        run_script('patches.py', '--out', out, '--fps', fps, '--extra', env.get('BB_PATCHES', ''),
                   '--settings', config, '--game-dir', game, '--render-res', env.get('BB_RENDER_RES', ''),
                   '--output-res', env.get('BB_OUTPUT_RES', ''),
                   '--patches-dir', env.get('BB_PATCHES_DIR', data / 'patches'),
                   '--patches-config', env.get('BB_PATCHES_CONFIG', data / 'patches.json'))
        # The GPU library's defaults as run.sh sets them (its comments say why).
        env.setdefault('BB_PREUPLOAD', '1')
        if not env.get('BB_PC_MODEL'):
            name = tool_output([str(caps), '--device']).partition('\t')[2]
            env['BB_PC_MODEL'] = '1'
            print(f'Memory model: new ({name or "GPU unknown"}; BB_PC_MODEL=0: the 0.3 model)')
        if env.get('BB_AS_0_3') == '1':
            env.update(BB_GUEST_IN_PLACE='0', BB_HOST_COPY_WAITS='all', BB_PRODUCER_CHECK='1')
        env.setdefault('BB_GUEST_IN_PLACE', env.get('BB_PC_MODEL', '0'))
        env.setdefault('BB_COPY_GPU_BUFFERS', '1')
        env.setdefault('BB_GPU_WRITE_TWINS', '1')
        env.setdefault('BB_GPU_WRITE_TWINS_MAX', '65536')
        if not env.get('BB_VBLANK_HZ'):
            env['BB_VBLANK_HZ'] = {'uncap': '1000', '90': '90', '120': '120'}.get(fps, '60')  # bbcoop: 120
        # bbcoop: uncapped frames are limited to 120 FPS unless BB_FPS_LIMIT is set (0 = no limit);
        # the fixed-frame-rate patches tune the camera's chase rates themselves (runtime_camera.c).
        if fps == 'uncap':
            env.setdefault('BB_FPS_LIMIT', '120')
        if fps != 'uncap' and (fps == '30' or env.get('BB_FRAME_HOOKS', '1') == '0'):
            env.setdefault('BB_CAMERA_RATES', '0')
        if env.get('BB_ONLINE') == '1':
            online_setup(env, user_dir, probe_done)
        command = [str(probe), str(out / 'boot-linked.bin'), '--content-profile', str(out / 'content.bin'),
                   '--patches', str(out / 'patches.bin'), '--app0', str(game), '--user', str(user_dir),
                   '--timeout', env.get('BB_TIMEOUT', '0'), *args]
        # No stdin: an inherited pipe (shells such as Git Bash) cost the game its console output.
        return subprocess.run(command, stdin=subprocess.DEVNULL, creationflags=no_console()).returncode
    finally:
        probe_done.set()
        if mod_view:
            # Read-only game files leave read-only links (mods.remove_overlay clears them).
            sys.path.insert(0, str(PORT / 'scripts'))
            from mods import remove_overlay
            try:
                remove_overlay(mod_view, original_game)
            except OSError:
                pass


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
