#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Bloodborne launcher for Windows and macOS (Tkinter; launcher/bbport_launcher.py is the Linux one).

Every setting of the port in one window: the game folder and saves, bbport.ini (upscaler,
preset, output, effects), start-up options passed to run.py as environment variables
(frame rate, presentation, HDR, ...), mods, third-party patches and the FSR 4 assets.
Launcher options live in %APPDATA%/bbcoop-launcher/settings.json (macOS: ~/Library/Application
Support/bbcoop-launcher).

bbcoop: Supermedo's launcher (github.com/Supermedo/bloodborne_pc, 1.9.2) without what bbcoop's runtime
does not have (docs/bbcoop/windows.md lists it), with the Linux launcher's co-op pools and Host/Join
(launcher/bbcoop_online.py). On macOS it starts run.sh; the Mac app (packaging/macos/package.sh)
keeps the port in Contents/Resources/port and its data in ~/Library/Application Support/bbcoop.

Frozen with PyInstaller (packaging/windows/package.sh) the same Bloodborne.exe also runs the
game without the window (`--play`), run.py (`--run`) and the preparation scripts (`--script`),
so a packaged port needs no Python installation.
"""
import ctypes
import json
import os
from pathlib import Path
import queue
import runpy
import signal
import subprocess
import sys
import threading
import time
import urllib.request
import webbrowser

FROZEN = getattr(sys, 'frozen', False)
MAC = sys.platform == 'darwin'  # bbcoop
PORT_DIR = Path(sys.executable).resolve().parent if FROZEN else Path(__file__).resolve().parent.parent
if FROZEN and MAC:  # bbcoop: Bloodborne.app/Contents/MacOS/Bloodborne
    PORT_DIR = PORT_DIR.parent / 'Resources' / 'port'
sys.path.insert(0, str(PORT_DIR / 'scripts'))
SUPPORT = Path.home() / 'Library' / 'Application Support'
DATA_DIR = Path(os.environ.get('BB_DATA_DIR', SUPPORT / 'bbcoop' if FROZEN and MAC else PORT_DIR))
# bbcoop: its own settings, apart from a bbport launcher on the same PC.
CONFIG_DIR = SUPPORT / 'bbcoop-launcher' if MAC else Path(os.environ.get('APPDATA', Path.home())) / 'bbcoop-launcher'
CONFIG_FILE = CONFIG_DIR / 'settings.json'
PATCH_VERSION = '01.09'
MAX_LOG_LINES = 6000
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
# bbcoop: how the game starts; elsewhere than Windows in its own process group (GameJob).
SPAWN = {'creationflags': NO_WINDOW} if sys.platform == 'win32' else {'start_new_session': True}
# This build; GitHub release tags are windows-v<VERSION>.
VERSION = 'bbcoop'
# Never copied over an installation by an update (the package does not hold them either).
USER_FILES = ('user', 'out', 'mods', 'bbport.ini', 'input.ini', 'mods.json', 'patches.json', 'last_run.log')


# ---------------------------------------------------------------------------------------------
# Command line roles of the frozen executable.

def attach_stdio():
    """A windowed executable starts without sys.stdout; inherited pipes or a console still exist."""
    if sys.platform != 'win32':  # bbcoop: the Mac app's are inherited
        return
    import msvcrt
    for name, std in (('stdout', -11), ('stderr', -12)):
        if getattr(sys, name) is not None:
            continue
        stream = None
        handle = ctypes.windll.kernel32.GetStdHandle(std)
        if handle and handle != ctypes.c_void_p(-1).value:
            try:
                stream = open(msvcrt.open_osfhandle(handle, os.O_WRONLY), 'w', encoding='utf-8',
                              errors='replace', buffering=1)
            except OSError:
                stream = None
        setattr(sys, name, stream or open(os.devnull, 'w'))


class LossyStream:
    """Output whose reader may be gone (a launcher that was closed): on Windows a write to such a
    pipe fails with OSError [Errno 22], which stopped the game while it was being prepared."""

    def __init__(self, stream):
        self.stream = stream

    def write(self, text):
        try:
            return self.stream.write(text)
        except (OSError, ValueError):
            return len(text)

    def flush(self):
        try:
            self.stream.flush()
        except (OSError, ValueError):
            pass

    def __getattr__(self, name):
        return getattr(self.stream, name)


def run_role(argv):
    """--run [args]: run.py; --script <file> [args]: a preparation script. Returns an exit code."""
    attach_stdio()
    for stream in (sys.stdout, sys.stderr):  # keep messages in order with the game's output
        try:
            stream.reconfigure(line_buffering=True)
        except (AttributeError, ValueError):
            pass
    sys.stdout, sys.stderr = LossyStream(sys.stdout), LossyStream(sys.stderr)
    if FROZEN:  # bbcoop: soulstruct's log lines link to their source file, which frozen code names relatively
        import logging
        make_record = logging.getLogRecordFactory()

        def absolute_record(*args, **kwargs):
            record = make_record(*args, **kwargs)
            record.pathname = os.path.abspath(record.pathname)
            return record
        logging.setLogRecordFactory(absolute_record)
    if argv[0] == '--script':
        args = argv[1:]
        while args and args[0].startswith('-') and args[0] != '-c':  # bbcoop: run.sh's `python3 -I script` (Mac app)
            args = args[1:]
        if args and args[0] == '-c':  # bbcoop: run.sh's `python3 -c code args`
            sys.argv = ['-c', *args[2:]]
            try:
                exec(compile(args[1], '<string>', 'exec'), {'__name__': '__main__'})
            except SystemExit as stop:
                return stop.code if isinstance(stop.code, int) else (0 if stop.code is None else 1)
            return 0
        path, sys.argv = args[0], args
        sys.path.insert(0, str(Path(path).resolve().parent))  # its neighbours, as `python3 script`
    else:
        path = str(PORT_DIR / 'run.py')
        sys.argv = [path, *argv[1:]]
    try:
        runpy.run_path(path, run_name='__main__')
    except SystemExit as stop:
        return stop.code if isinstance(stop.code, int) else (0 if stop.code is None else 1)
    return 0


def run_command():
    """The command that starts run.py: this executable when frozen, else Python. bbcoop: run.sh on macOS."""
    if MAC:
        return ['/bin/bash', str(PORT_DIR / 'run.sh')]
    return [sys.executable, '--run'] if FROZEN else [sys.executable, str(PORT_DIR / 'run.py')]


def python_command():
    """bbcoop: how a script of the port runs (the host's game server): this executable when frozen."""
    return [sys.executable, '--script'] if FROZEN else [sys.executable]


# ---------------------------------------------------------------------------------------------
# Languages: every text is written in English with the Russian next to it; the other
# languages are in bbport_lang.py, keyed by the English text.

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bbport_lang  # noqa: E402
# bbcoop: the co-op addresses are scripts/online.py's (run.py and the Linux launcher use it too).
sys.path.insert(0, str(PORT_DIR / 'scripts'))
import online  # noqa: E402
import bbcoop_online  # noqa: E402

LANG = 'en'


WARN = '  ⚠'  # marks a risky choice; the text before it is translated as usual


def _(en, ru=None):
    if en.endswith(WARN):
        return _(en[:-len(WARN)], ru and ru.removesuffix(WARN)) + WARN
    if LANG == 'ru':
        return ru or bbport_lang.table('ru').get(en) or en
    return bbport_lang.table(LANG).get(en) or en


def windows_language():
    try:
        primary = ctypes.windll.kernel32.GetUserDefaultUILanguage() & 0x3ff
    except (AttributeError, OSError):
        return 'en'
    return bbport_lang.WINDOWS_LANGUAGES.get(primary, 'en')


# ---------------------------------------------------------------------------------------------
# Settings. bbport.ini keys (the game reads them, the in-game menu edits them) and the
# launcher's own settings.json (passed to run.py as environment variables).

EFFECTS = [
    ('effect_chromatic_aberration', ('Chromatic aberration', 'Хроматическая аберрация'), True),
    ('effect_dof', ('Depth of field', 'Глубина резкости (DoF)'), True),
    ('effect_motion_blur', ('Motion blur', 'Размытие в движении'), True),
    ('effect_ssao', ('Ambient occlusion (SSAO)', 'Затенение SSAO'), True),
    ('effect_game_aa', ("The game's own anti-aliasing", 'Собственное сглаживание игры'), True),
    ('effect_dynamic_shadows', ('Shadows of dynamic lights', 'Тени от динамических источников'), True),
    ('effect_ssr', ('Screen-space reflections (not in the original)', 'Отражения SSR (не было в игре)'), False),
]
EXTRAS = [
    ('skip_intro', ('Skip the intro logos and movie', 'Пропуск заставок при запуске'), False),
    ('debug_camera', ('Free camera (hold Cross + L3; keyboard Space + Z)', 'Свободная камера (Cross + L3 / Space + Z)'), False),
    ('debug_menu', ('Game debug menu (left touchpad / Tab; needs the debug fonts)',
                    'Debug menu (левый touchpad / Tab; нужны шрифты)'), False),
]
INI_FLAGS = {'sharpen', 'object_motion', 'show_fps', *(k for k, _t, _o in EFFECTS + EXTRAS)}
INI_DEFAULTS = {'upscaler': 'fsr4', 'preset': '1', 'sharpen': '1', 'sharpness': '0.50',
                'object_motion': '1', 'show_fps': '1', 'output_res': '1920x1080', 'model_lod': '0',
                'live_resolution': 'auto',
                **{key: '1' if on else '0' for key, _t, on in EFFECTS + EXTRAS}}
APP_DEFAULTS = {'ui_language': '', 'game_dir': str(PORT_DIR.parent / 'CUSA03173'), 'user_dir': '',
                'mods_dir': '', 'mods_enabled': True, 'patches_dir': '', 'language': '1',
                'player_name': '', 'fullscreen': False, 'hdr': False, 'present_mode': 'Mailbox',
                'fps_mode': 'uncap', 'frame_cap': '', 'draw_pipe': '', 'readbacks': '',
                'frames_ahead': '', 'frame_stats': False, 'gpu_profile': False,
                'vk_validation': False, 'extra_env': '', 'close_on_play': False,
                'online': False, 'online_server': online.SHADNET_SERVER,
                'online_webapi': '', 'online_npid': '', 'online_password': '', 'online_upnp': True,
                'online_community': online.COMMUNITY_SERVER, 'online_pool': 'seamless', 'online_bbsc': True,
                'thd_online_id': '', 'thd_token': '',
                # bbcoop: Host/Join ('manual': the server fields above); the host joined.
                'coop_mode': 'manual', 'coop_join': ''}
# bbbridge's own app reads and writes the online_* keys above: keep their names.


UPSCALERS = [('dlss', ('DLSS (NVIDIA GeForce RTX)',)),
             ('fsr4', ('FSR 4 (best quality)', 'FSR 4 (лучшее качество)')),
             ('fsr411', ('FSR 4.1.1 (needs fsr4_411 assets)', 'FSR 4.1.1 (нужны ассеты fsr4_411)')),
             ('fsr3', ('FSR 3.1 (every GPU)', 'FSR 3.1 (любая видеокарта)')),
             ('taa', ('TAA (native resolution anti-aliasing)', 'TAA (нативное сглаживание)')),
             ('off', ('Off', 'Выключен'))]
if MAC:  # bbcoop: what an M4 Air runs (CLAUDE.md, "macOS"): 30 FPS, 720p native; Apple's GPUs have no DLSS or FSR 4
    INI_DEFAULTS.update(upscaler='off', preset='0', output_res='1280x720')
    APP_DEFAULTS.update(fps_mode='30')
    UPSCALERS = [('off', ('Off',)), ('metalfx', ('MetalFX (Apple)',)), ('fsr3', ('FSR 3.1',)),
                 ('taa', ('TAA (native resolution anti-aliasing)',))]
PRESETS = [('0', ('Native AA (×1.0)',)), ('1', ('Quality (×1.5)',)), ('2', ('Balanced (×1.7)',)),
           ('3', ('Performance (×2)',)), ('4', ('Ultra Performance (×3)',))]
OUTPUTS = [('1280x720', ('1280 × 720 (Steam Deck)',)), ('1920x1080', ('1920 × 1080',)),
           ('2560x1440', ('2560 × 1440',)), ('3840x2160', ('3840 × 2160 (4K)',)),
           ('2560x1080', ('2560 × 1080 (21:9)',)), ('3440x1440', ('3440 × 1440 (21:9)',)),
           ('3840x1600', ('3840 × 1600 (21:9)',)), ('5120x2160', ('5120 × 2160 (21:9)',)),
           ('3840x1080', ('3840 × 1080 (32:9)',)), ('5120x1440', ('5120 × 1440 (32:9)',))]


LIVE = [('auto', ('Auto (by graphics card)', 'Авто (по видеокарте)')), ('0', ('Off (faster)', 'Выключена (быстрее)')),
        ('1', ('On (change without restarting)', 'Включена (без перезапуска)'))]
LODS = [('0', ('As in the game', 'Как в игре')), ('-2', ('Highest (−2)', 'Максимальная (−2)')),
        ('1', ('Lower (1)', 'Ниже (1)')), ('2', ('Lowest (2)', 'Минимальная (2)'))]
FPS_MODES = [('uncap', ('Unlocked (frame-time patch)', 'Без ограничения (патч)')), ('60', ('60 FPS',)),
             ('90', ('90 FPS',)), ('120', ('120 FPS',)), ('30', ('30 FPS (as on PS4)', '30 FPS (как на PS4)'))]
PRESENT_MODES = [('Mailbox', ('Mailbox (low latency, no tearing)', 'Mailbox (без разрывов)')),
                 ('Fifo', ('FIFO (VSync)',)), ('FifoRelaxed', ('FIFO Relaxed',)),
                 ('Immediate', ('Immediate (tearing)', 'Immediate (с разрывами)'))]
LANGUAGES = [('1', ('English', 'Английский')), ('8', ('Russian', 'Русский')), ('0', ('Japanese', 'Японский')),
             ('2', ('French', 'Французский')), ('3', ('Spanish', 'Испанский')), ('4', ('German', 'Немецкий')),
             ('5', ('Italian', 'Итальянский')), ('17', ('Português (Brasil)',))]
DRAW_PIPE = [('', ('Auto (8+ threads)', 'Авто (8+ потоков)')), ('1', ('On', 'Включён')),
             ('0', ('Off (more stable)', 'Выключен (стабильнее)'))]
READBACKS = [('', ('Relaxed (default)', 'Relaxed (по умолчанию)')), ('0', ('Off', 'Выключены')),
             ('2', ('Precise',))]
# Frame cap of the unlocked mode (BB_FPS_LIMIT). '' leaves the port's own: the display refresh,
# at most 120, because the game's movement timing breaks above about 120 FPS.
FRAME_CAPS = [('', ('Auto: display refresh, max 120 (recommended)', 'Авто: частота монитора, макс. 120 (рекомендуется)')),
              ('60', ('60',)), ('90', ('90',)), ('120', ('120',)), ('144', ('144  ⚠',)), ('165', ('165  ⚠',)),
              ('240', ('240  ⚠',)), ('0', ('No limit  ⚠', 'Без ограничения  ⚠'))]
FRAMES_AHEAD = [('', ('1 (default)', '1 (по умолчанию)')), ('2', ('2',)), ('0', ('Unbounded', 'Без ограничения'))]
UI_LANGUAGES = bbport_lang.LANGUAGE_NAMES

FSR4_COMMIT = 'ae8d628fae208813172446d1e49ed94150b04658'
FSR4_BASE = f'https://raw.githubusercontent.com/FireBurn/Q2RTX/{FSR4_COMMIT}/baseq2/fsr4_shaders'


def fsr4_files():
    """The FSR 4 v07 asset set of tools/fetch_fsr4_assets.sh (1080 and 2160 tiers)."""
    files = ['LICENSE-FSR4-v07.txt', 'rcas.spv', 'spd_auto_exposure.spv']
    for model in ('native', 'quality', 'balanced', 'performance', 'ultraperf', 'drs'):
        files += [f'fsr4_model_v07_i8_{model}_initializers.bin', f'fsr4_model_v07_i8_{model}_pre_weights.bin',
                  f'fsr4_model_v07_i8_{model}_shader_manifest.json']
        for tier in ('1080', '2160'):
            files += [f'fsr4_model_v07_i8_{model}_{tier}_pre.spv', f'fsr4_model_v07_i8_{model}_{tier}_post.spv']
            files += [f'fsr4_model_v07_i8_{model}_{tier}_pass{n}.spv' for n in range(1, 13)]
    return files


def fsr4_missing():
    folder = PORT_DIR / 'fsr4_shaders'
    return [name for name in fsr4_files() if not (folder / name).is_file() or not (folder / name).stat().st_size]


def load_json(path, default):
    try:
        return json.loads(Path(path).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return default


def ini_path():
    return Path(os.environ.get('BB_CONFIG', DATA_DIR / 'bbport.ini'))


def load_ini():
    values, lines = dict(INI_DEFAULTS), []
    try:
        lines = ini_path().read_text(encoding='utf-8').splitlines()
    except OSError:
        pass
    for line in lines:
        if '=' in line and not line.lstrip().startswith('#'):
            key, value = line.split('=', 1)
            values[key.strip()] = value.strip()
    return values, lines


def save_ini(values, lines):
    """Rewrites the edited keys in place, appends missing ones, keeps comments and other keys."""
    written, out = set(), []
    for line in lines:
        if '=' in line and not line.lstrip().startswith('#'):
            key = line.split('=', 1)[0].strip()
            if key in values:
                out.append(f'{key}={values[key]}')
                written.add(key)
                continue
        out.append(line)
    if not lines:
        out.append('# bbport settings (in-game menu: Insert / L3+R3)')
    out += [f'{key}={value}' for key, value in values.items() if key not in written]
    ini_path().write_text('\n'.join(out) + '\n', encoding='utf-8')


def game_info(folder):
    """(title, app version) of a game folder, or None without eboot.bin."""
    folder = Path(folder or '.')
    if not (folder / 'eboot.bin').is_file():
        return None
    try:
        from prepare import sfo
        values = sfo((folder / 'sce_sys/param.sfo').read_bytes())
        # bbcoop: some 1.09 dumps (CUSA00900) keep APP_VER 01.00 and carry the update in VERSION.
        version = PATCH_VERSION if values.get('VERSION') == PATCH_VERSION else values.get('APP_VER', '?')
        return values.get('TITLE', 'Bloodborne').replace('™', '').strip(), version
    except (OSError, ValueError, ImportError):
        return 'Bloodborne', '?'


def game_environment(s):
    env = dict(os.environ)
    env['BB_GAME_DIR'] = s['game_dir']
    if s['user_dir']:
        env['BB_USER_DIR'] = s['user_dir']
    env['BB_MODS_DIR'] = s['mods_dir'] or str(DATA_DIR / 'mods')
    env['BB_MODS_CONFIG'] = str(DATA_DIR / 'mods.json')
    env['BB_MODS_ENABLED'] = '1' if s['mods_enabled'] else '0'
    env['BB_PATCHES_DIR'] = s['patches_dir'] or str(DATA_DIR / 'patches')
    env['BB_PATCHES_CONFIG'] = str(DATA_DIR / 'patches.json')
    env['BB_LANGUAGE'] = s['language']
    if str(s['player_name']).strip():
        env['BB_USER_NAME'] = str(s['player_name']).strip()
    env['BB_FULLSCREEN'] = '1' if s['fullscreen'] else '0'
    env['BB_PRESENT_MODE'] = s['present_mode']
    if s['hdr']:
        env['BB_HDR'] = '1'
    env['BB_FPS'] = s['fps_mode']
    if s.get('frame_cap', ''):
        env['BB_FPS_LIMIT'] = s['frame_cap']
    for key, name in (('draw_pipe', 'BB_DRAW_PIPE'), ('readbacks', 'BB_READBACKS'), ('frames_ahead', 'BB_FRAMES_AHEAD')):
        if s[key]:
            env[name] = s[key]
    for key, name in (('frame_stats', 'BB_FRAME_STATS'), ('gpu_profile', 'BB_GPU_PROFILE'),
                      ('vk_validation', 'BB_VK_VALIDATION')):
        if s[key]:
            env[name] = '1'
    for item in str(s['extra_env']).split():
        if '=' in item:
            key, value = item.split('=', 1)
            env[key] = value
    if s.get('online') and not bbcoop_online.join_without_host(s):  # bbcoop: no host: offline
        # bbcoop: as the Linux launcher: Seamless Hunt (BBSC) or The Hunter's Dream, shadPS4 0.20's module.
        server = str(s['online_server']).strip() or online.SHADNET_SERVER
        community = online.community_address(s['online_community'])
        env['BB_ONLINE'] = '1'
        env['BB_SKIP_NETWORK_CHOICE'] = '0'  # the skip patch picks offline (scripts/patches.py)
        env['BB_SHADNET_SERVER'] = server
        env['BB_SHADNET_WEBAPI'] = online.webapi_setting(s['online_webapi'], server, community)
        env['BB_SHADNET_NPID'] = str(s['online_npid']).strip()
        env['BB_SHADNET_PASSWORD'] = str(s['online_password'])
        env['BB_UPNP'] = '1' if s['online_upnp'] else '0'
        env['BB_COMMUNITY_SERVER'] = community
        env['BB_NET_GEN'] = '20'
        if s.get('online_pool') == 'huntersdream':
            # run.py turns these into the backend's addresses and the host overrides.
            env['BB_NET_BACKEND'] = 'thd'
            env['BB_COMMUNITY_SERVER'] = online.thd_base('')
            env['BB_THD_ONLINE_ID'] = str(s.get('thd_online_id', ''))
            env['BB_THD_TOKEN'] = str(s.get('thd_token', ''))
        elif s.get('online_bbsc'):
            env['BB_BBSC'] = '1'
    if MAC and FROZEN:  # bbcoop: the Mac app's prebuilt port; this executable runs its scripts
        python = str(PORT_DIR / 'bin' / 'python3')
        env.update(BB_PREBUILT='1', BB_DATA_DIR=str(DATA_DIR), PYTHON=python, BB_RE_PYTHON=python,
                   BB_MOLTENVK_ICD=str(PORT_DIR / 'bin' / 'gpu' / 'MoltenVK_icd.json'))
    env['PYTHONUNBUFFERED'] = '1'
    env['PYTHONIOENCODING'] = 'utf-8'
    return env


# ---------------------------------------------------------------------------------------------
# The window.

BG, PANEL, CARD, LINE = '#0e0c0b', '#151210', '#1c1815', '#2e2722'
TEXT, MUTED, GOLD, BLOOD, BLOOD_HI = '#e9e2d6', '#9a8f80', '#c8a96a', '#7c1717', '#9e2222'


class Launcher:
    def __init__(self, root, tk, ttk, filedialog, messagebox):
        self.tk, self.ttk, self.filedialog, self.messagebox = tk, ttk, filedialog, messagebox
        self.root = root
        self.app = {**APP_DEFAULTS, **load_json(CONFIG_FILE, {})}
        self.ini, self.ini_lines = load_ini()
        self.vars = {}
        self.process = self.job = None
        self.hidden = False  # closed while the game runs: it exits with the game
        self.downloading = False
        self.output = queue.Queue()
        self.gpu_text = _('• Checking the graphics card…', '• Проверка видеокарты…')
        self.banner_source = self.banner_image = None
        self.ui_calls = queue.Queue()  # work for the Tk thread from helper threads
        self.mod_order, self.mod_vars, self.patch_vars = [], {}, {}
        # bbcoop: Host/Join (launcher/bbcoop_online.py): the server while hosting, this build.
        self.host = bbcoop_online.Host(PORT_DIR, DATA_DIR, python_command())
        self.build_id = bbcoop_online.build_id(PORT_DIR)
        self.hosts_found, self.host_lock, self.host_note = [], threading.Lock(), ''
        root.title('Bloodborne — bbcoop')
        root.configure(bg=BG)
        if MAC:  # bbcoop: Tk's macOS points are 72 per inch; the layout is drawn for 96
            root.tk.call('tk', 'scaling', 96 / 72)
        self.dpi = root.winfo_fpixels('1i') / 96.0
        root.geometry(f'{self.px(1120)}x{self.px(740)}')
        root.minsize(self.px(980), self.px(660))
        self.set_icon()
        self.style()
        self.build()
        self.ini_shown = self.ini_values()  # bbcoop: what collect() compares against
        self.show('play')
        root.protocol('WM_DELETE_WINDOW', self.close)
        root.after(100, self.drain_output)
        root.after(300, self.first_start)
        root.after(10000, self.watch_host)  # bbcoop
        threading.Thread(target=self.detect_gpu, daemon=True).start()

    def px(self, size):
        return int(size * self.dpi)

    def set_icon(self):
        try:
            self.root.iconbitmap(default=str(PORT_DIR / 'launcher' / 'bloodborne.ico'))
            return
        except self.tk.TclError:
            pass
        for icon in (PORT_DIR / 'launcher' / 'bloodborne.png', Path(self.app['game_dir']) / 'sce_sys' / 'icon0.png'):
            try:
                self.icon = self.tk.PhotoImage(file=str(icon))
                if self.icon.width() > 128:
                    self.icon = self.icon.subsample(self.icon.width() // 64)
                self.root.iconphoto(True, self.icon)
                return
            except (self.tk.TclError, OSError):
                continue

    # ---- look --------------------------------------------------------------------------------
    def check_images(self):
        """16 px (DPI-scaled) check box images: the clam theme draws a cross."""
        tk = self.tk
        n = max(14, int(16 * self.dpi))
        images = []
        for checked in (False, True):
            image = tk.PhotoImage(width=n + self.px(8), height=n)  # unset pixels stay transparent
            fill = BLOOD if checked else CARD
            image.put(GOLD if checked else '#5a4c40', to=(0, 0, n, n))
            image.put(fill, to=(1, 1, n - 1, n - 1))
            if checked:
                # Tick: down from (0.22n, 0.52n) to (0.42n, 0.72n), up to (0.78n, 0.30n).
                pts = []
                x0, y0, x1, y1, x2, y2 = .22 * n, .52 * n, .42 * n, .72 * n, .78 * n, .28 * n
                for t in range(40):
                    f = t / 39
                    pts.append((x0 + (x1 - x0) * f, y0 + (y1 - y0) * f))
                    pts.append((x1 + (x2 - x1) * f, y1 + (y2 - y1) * f))
                width = max(1, round(n / 9))
                for x, y in pts:
                    image.put('#f4ece0', to=(int(x), int(y), int(x) + width, int(y) + width))
            images.append(image)
        self.check_off, self.check_on = images

    def style(self):
        ttk = self.ttk
        s = ttk.Style(self.root)
        s.theme_use('clam')
        self.check_images()
        s.element_create('Bb.indicator', 'image', self.check_off, ('selected', self.check_on), sticky='')
        s.layout('TCheckbutton', [('Checkbutton.padding', {'sticky': 'nswe', 'children': [
            ('Bb.indicator', {'side': 'left', 'sticky': ''}),
            ('Checkbutton.focus', {'side': 'left', 'sticky': 'w', 'children': [
                ('Checkbutton.label', {'sticky': 'nswe'})]})]})])
        base = ('Segoe UI', 10)
        self.root.option_add('*TCombobox*Listbox.background', CARD)
        self.root.option_add('*TCombobox*Listbox.foreground', TEXT)
        self.root.option_add('*TCombobox*Listbox.selectBackground', BLOOD)
        self.root.option_add('*TCombobox*Listbox.font', base)
        s.configure('.', background=PANEL, foreground=TEXT, fieldbackground=CARD, bordercolor=LINE,
                    lightcolor=LINE, darkcolor=LINE, troughcolor=CARD, focuscolor=GOLD, font=base)
        s.configure('TFrame', background=PANEL)
        s.configure('TLabel', background=PANEL, foreground=TEXT)
        s.configure('Muted.TLabel', background=PANEL, foreground=MUTED, font=('Segoe UI', 9))
        s.configure('Section.TLabel', background=PANEL, foreground=GOLD, font=('Georgia', 13))
        s.configure('Code.TLabel', background=PANEL, foreground=GOLD, font=('Consolas', 22, 'bold'))
        s.configure('TCheckbutton', background=PANEL, foreground=TEXT, padding=(0, 3))
        s.map('TCheckbutton', background=[('active', PANEL)], foreground=[('disabled', MUTED)])
        s.configure('Warning.TLabel', background='#2a1d12', foreground='#e3b25a', padding=(10, 6))
        s.configure('TCombobox', arrowcolor=GOLD, foreground=TEXT, padding=4)
        s.map('TCombobox', fieldbackground=[('readonly', CARD)], foreground=[('readonly', TEXT)],
              selectbackground=[('readonly', CARD)], selectforeground=[('readonly', TEXT)])
        s.configure('TEntry', foreground=TEXT, insertcolor=TEXT, padding=4)
        s.configure('TSpinbox', foreground=TEXT, arrowcolor=GOLD, insertcolor=TEXT, padding=4)
        s.configure('TButton', background=CARD, foreground=TEXT, padding=(12, 6), borderwidth=1)
        s.map('TButton', background=[('active', LINE), ('disabled', PANEL)], foreground=[('disabled', MUTED)])
        # The Controls page's binding slots; they stay as they are while another slot listens.
        s.configure('Slot.TButton', background=CARD, foreground=TEXT, padding=(6, 5), font=('Segoe UI', 9))
        s.map('Slot.TButton', background=[('active', LINE), ('disabled', CARD)], foreground=[('disabled', TEXT)])
        s.configure('Listen.TButton', background=LINE, foreground=GOLD, padding=(6, 5), font=('Segoe UI', 9))
        s.map('Listen.TButton', background=[('disabled', LINE)], foreground=[('disabled', GOLD)])
        s.configure('Play.TButton', background=BLOOD, foreground='#f4ece0', font=('Georgia', 15, 'bold'),
                    padding=(36, 10), borderwidth=0)
        s.map('Play.TButton', background=[('active', BLOOD_HI), ('disabled', '#3a2420')],
              foreground=[('disabled', '#8a7a70')])
        s.configure('Horizontal.TProgressbar', background=GOLD, troughcolor=CARD, bordercolor=LINE)
        s.configure('Vertical.TScrollbar', background=CARD, arrowcolor=GOLD, troughcolor=PANEL, bordercolor=PANEL)

    # ---- widget helpers ----------------------------------------------------------------------
    def var(self, key, store):
        """The Tk variable of a setting (one per setting, shared by every widget that shows it)."""
        tk = self.tk
        if key not in self.vars:
            if store == 'ini':
                value = self.ini.get(key, INI_DEFAULTS[key])
                if key in INI_FLAGS:
                    v = tk.BooleanVar(value=value == '1')
                elif key == 'sharpness':
                    v = tk.DoubleVar(value=float(value or 0.5))
                else:
                    v = tk.StringVar(value=value)
            else:
                value = self.app.get(key, APP_DEFAULTS[key])
                if isinstance(APP_DEFAULTS[key], bool):
                    v = tk.BooleanVar(value=bool(value))
                else:
                    v = tk.StringVar(value=str(value))
            v.store = store
            self.vars[key] = v
        return self.vars[key]

    def choice(self, parent, key, store, options, width=38):
        """A combobox over (value, (english, russian)) pairs, kept in sync with its variable."""
        var = self.var(key, store)
        values = [v for v, _t in options]
        box = self.ttk.Combobox(parent, values=[_(*text) for _v, text in options], state='readonly', width=width)
        if var.get() not in values:
            var.set(values[0])

        def show(*_args):
            current = var.get()
            box.current(values.index(current) if current in values else 0)
        show()
        box.bind('<<ComboboxSelected>>', lambda _e: var.set(values[box.current()]))
        var.trace_add('write', show)
        return box

    def next_row(self, parent):
        return parent.grid_size()[1]

    def row(self, parent, title, widget, hint=None):
        ttk = self.ttk
        r = self.next_row(parent)
        ttk.Label(parent, text=title).grid(row=r, column=0, sticky='nw', padx=(0, 18), pady=(8, 0))
        widget.grid(row=r, column=1, sticky='w', pady=(5, 0))
        if hint:
            ttk.Label(parent, text=hint, style='Muted.TLabel', wraplength=self.px(560), justify='left').grid(
                row=r + 1, column=1, sticky='w', pady=(2, 2))
        return widget

    def check(self, parent, key, store, title, hint=None):
        ttk = self.ttk
        r = self.next_row(parent)
        ttk.Checkbutton(parent, text=title, variable=self.var(key, store)).grid(
            row=r, column=0, columnspan=2, sticky='w', pady=(4, 0))
        if hint:
            ttk.Label(parent, text=hint, style='Muted.TLabel', wraplength=self.px(640), justify='left').grid(
                row=r + 1, column=0, columnspan=2, sticky='w', padx=(26, 0))

    def note(self, parent, text, top=12):
        self.ttk.Label(parent, text=text, style='Muted.TLabel', wraplength=self.px(680), justify='left').grid(
            row=self.next_row(parent), column=0, columnspan=2, sticky='w', pady=(top, 0))

    def section(self, parent, title, top=18):
        self.ttk.Label(parent, text=title, style='Section.TLabel').grid(
            row=self.next_row(parent), column=0, columnspan=2, sticky='w', pady=(top, 2))

    def folder(self, parent, key, title, prompt, hint=None, on_change=None):
        ttk = self.ttk
        var = self.var(key, 'app')
        holder = ttk.Frame(parent)
        ttk.Entry(holder, textvariable=var, width=54).pack(side='left')

        def browse():
            chosen = self.filedialog.askdirectory(title=prompt, initialdir=var.get() or str(PORT_DIR))
            if chosen:
                var.set(str(Path(chosen)))
        ttk.Button(holder, text=_('Browse…', 'Обзор…'), command=browse).pack(side='left', padx=(6, 0))
        ttk.Button(holder, text=_('Open', 'Открыть'), command=lambda: self.open_path(var.get(), key)).pack(
            side='left', padx=(6, 0))
        if on_change:
            var.trace_add('write', lambda *_a: on_change())
        return self.row(parent, title, holder, hint)

    def scrolled_page(self, name, title, subtitle):
        """A settings page: a heading and content that scrolls vertically."""
        tk, ttk = self.tk, self.ttk
        outer = ttk.Frame(self.content)
        head = ttk.Frame(outer, padding=(28, 22, 28, 6))
        head.pack(fill='x')
        ttk.Label(head, text=title, background=PANEL, foreground=TEXT, font=('Georgia', 20)).pack(anchor='w')
        ttk.Label(head, text=subtitle, style='Muted.TLabel').pack(anchor='w')
        canvas = tk.Canvas(outer, bg=PANEL, highlightthickness=0, bd=0)
        bar = ttk.Scrollbar(outer, orient='vertical', command=canvas.yview)
        inner = ttk.Frame(canvas, padding=(28, 0, 28, 24))
        inner.columnconfigure(1, weight=1)
        window = canvas.create_window(0, 0, window=inner, anchor='nw')
        inner.bind('<Configure>', lambda _e: canvas.configure(scrollregion=canvas.bbox('all')))
        canvas.bind('<Configure>', lambda e: canvas.itemconfigure(window, width=e.width))
        canvas.configure(yscrollcommand=bar.set)
        bar.pack(side='right', fill='y')
        canvas.pack(side='left', fill='both', expand=True)
        outer.scroll = lambda units: canvas.yview_scroll(units, 'units') if inner.winfo_height() > canvas.winfo_height() else None
        self.pages[name] = outer
        return inner

    # ---- layout ------------------------------------------------------------------------------
    def build(self):
        tk, ttk = self.tk, self.ttk
        side = tk.Frame(self.root, bg=BG, width=self.px(220))
        side.pack(side='left', fill='y')
        side.pack_propagate(False)
        tk.Label(side, text='BLOODBORNE', bg=BG, fg=GOLD, font=('Georgia', 16)).pack(anchor='w', padx=22, pady=(24, 0))
        self.side = side
        tk.Label(side, text='bbcoop', bg=BG, fg=TEXT, font=('Segoe UI', 10)).pack(anchor='w', padx=22)
        tk.Label(side, text=_('native port · macOS') if MAC else _('native port · Windows', 'нативный порт · Windows'),
                 bg=BG, fg=MUTED, justify='left',
                 font=('Segoe UI', 9)).pack(anchor='w', padx=22, pady=(0, 20))
        self.nav, self.current_page = {}, None
        for name, title in (('play', _('Play', 'Играть')), ('graphics', _('Graphics', 'Графика')),
                            ('display', _('Display & FPS', 'Экран и FPS')), ('game', _('Game & effects', 'Игра и эффекты')),
                            ('online', _('Online co-op', 'Онлайн-кооператив')),
                            ('mods', _('Mods & patches', 'Моды и патчи')),
                            ('advanced', _('Advanced', 'Дополнительно')),
                            ('log', _('Log', 'Журнал'))):
            item = tk.Label(side, text='    ' + title, bg=BG, fg=TEXT, anchor='w', font=('Segoe UI', 11),
                            pady=10, cursor='hand2')
            item.pack(fill='x')
            item.bind('<Button-1>', lambda _e, n=name: self.show(n))
            item.bind('<Enter>', lambda _e, n=name: n != self.current_page and self.nav[n].configure(bg='#1a1613'))
            item.bind('<Leave>', lambda _e, n=name: n != self.current_page and self.nav[n].configure(bg=BG))
            self.nav[name] = item
        tk.Frame(side, bg=BG).pack(fill='both', expand=True)
        self.side_note = tk.Label(side, text=_('In the game: Insert or L3+R3\nopens the port\'s menu.',
                                               'В игре: Insert или L3+R3\nоткрывает меню порта.'),
                                  bg=BG, fg=MUTED, font=('Segoe UI', 9), justify='left', wraplength=self.px(210))
        self.side_note.pack(anchor='w', padx=22, pady=(0, 18))

        right = tk.Frame(self.root, bg=PANEL)
        right.pack(side='left', fill='both', expand=True)
        bar = tk.Frame(right, bg=BG, height=72)
        bar.pack(fill='x', side='bottom')
        bar.pack_propagate(False)
        self.status = tk.Label(bar, text='', bg=BG, fg=MUTED, font=('Segoe UI', 10), anchor='w', justify='left')
        self.status.pack(side='left', padx=24)
        self.play_button = ttk.Button(bar, text=_('PLAY', 'ИГРАТЬ'), style='Play.TButton', command=self.play)
        self.play_button.pack(side='right', padx=(10, 24), pady=11)
        self.stop_button = ttk.Button(bar, text=_('Stop', 'Остановить'), command=self.stop, state='disabled')
        self.stop_button.pack(side='right', pady=11)
        self.content = tk.Frame(right, bg=PANEL)
        self.content.pack(fill='both', expand=True)

        self.pages = {}
        self.build_play()
        self.build_graphics()
        self.build_display()
        self.build_game()
        self.build_online()
        self.build_mods()
        self.build_advanced()
        self.build_log()
        self.root.bind_all('<MouseWheel>', self.wheel)
        self.refresh_status()

    def wheel(self, event):
        page = self.pages.get(self.current_page)
        if hasattr(page, 'scroll') and not isinstance(event.widget, (self.tk.Text, self.ttk.Combobox)) \
                and 'popdown' not in str(event.widget):
            page.scroll(-event.delta if MAC else int(-event.delta / 120))  # bbcoop: macOS sends lines

    def show(self, name):
        for page in self.pages.values():
            page.pack_forget()
        self.pages[name].pack(fill='both', expand=True)
        self.current_page = name
        for n, item in self.nav.items():
            item.configure(bg=PANEL if n == name else BG, fg=GOLD if n == name else TEXT)
        if name == 'mods':
            self.refresh_lists()
        elif name == 'graphics':
            self.refresh_fsr4()
        elif name == 'play':
            self.refresh_status()

    def build_play(self):
        tk, ttk = self.tk, self.ttk
        page = tk.Frame(self.content, bg=PANEL)
        self.pages['play'] = page
        self.banner = tk.Canvas(page, height=self.px(290), bg=BG, highlightthickness=0, bd=0)
        self.banner.pack(fill='x')
        self.banner.bind('<Configure>', lambda _e: self.draw_banner())
        body = ttk.Frame(page, padding=(28, 12, 28, 8))
        body.pack(fill='both', expand=True)
        body.columnconfigure(0, weight=3)
        body.columnconfigure(1, weight=2)
        info = ttk.Frame(body)
        info.grid(row=0, column=0, sticky='nw', padx=(0, 24))
        ttk.Label(info, text=_('Ready check', 'Проверка'), style='Section.TLabel').pack(anchor='w', pady=(0, 6))
        self.checks = {}
        for key in ('game', 'saves', 'gpu', 'fsr4'):
            self.checks[key] = ttk.Label(info, text='', justify='left', wraplength=self.px(440))
            self.checks[key].pack(anchor='w', pady=3)
        quick = ttk.Frame(body)
        quick.grid(row=0, column=1, sticky='nw')
        ttk.Label(quick, text=_('Quick settings', 'Основное'), style='Section.TLabel').grid(
            row=0, column=0, columnspan=2, sticky='w', pady=(0, 2))
        self.row(quick, _('Frame rate', 'Частота кадров'), self.choice(quick, 'fps_mode', 'app', FPS_MODES, 26))
        self.row(quick, _('Upscaler', 'Апскейлер'), self.choice(quick, 'upscaler', 'ini', UPSCALERS, 26))
        self.row(quick, _('Preset', 'Пресет'), self.choice(quick, 'preset', 'ini', PRESETS, 26))
        self.row(quick, _('Output', 'Разрешение'), self.choice(quick, 'output_res', 'ini', OUTPUTS, 26))
        ttk.Checkbutton(quick, text=_('Fullscreen', 'Полный экран'), variable=self.var('fullscreen', 'app')).grid(
            row=self.next_row(quick), column=1, sticky='w', pady=(8, 0))
        ttk.Checkbutton(quick, text=_('Play online'), variable=self.var('online', 'app')).grid(
            row=self.next_row(quick), column=1, sticky='w', pady=(4, 0))
        if not MAC:  # bbcoop: Steam's shortcuts here are Windows'
            ttk.Button(quick, text=_('Add to Steam'), command=self.add_to_steam).grid(
                row=self.next_row(quick), column=1, sticky='w', pady=(10, 0))
        for key in ('fps_mode', 'upscaler', 'output_res'):
            self.vars[key].trace_add('write', lambda *_a: self.refresh_status())

    def draw_banner(self):
        """The cover art of the selected dump (sce_sys/pic1.png) under the title."""
        tk, c = self.tk, self.banner
        c.delete('all')
        w, h = max(c.winfo_width(), 400), int(c['height'])
        art = Path(self.var('game_dir', 'app').get() or '.') / 'sce_sys' / 'pic1.png'
        if self.banner_source != art:
            self.banner_source, self.banner_image, self.banner_size = art, None, None
            try:
                self.banner_image = tk.PhotoImage(file=str(art))
            except (tk.TclError, OSError):
                pass
        if self.banner_image:
            if self.banner_size != w:
                self.banner_size, self.banner_scaled = w, self.scaled_art(art, w)
            # The logo sits in the upper half of the cover: show that part.
            c.create_image(w // 2, int(h * 0.62), image=self.banner_scaled)
            for i, stipple in enumerate(('gray12', 'gray25', 'gray50', 'gray75')):
                c.create_rectangle(0, h - 120 + i * 24, w, h - 96 + i * 24, fill=PANEL, outline='', stipple=stipple)
            c.create_rectangle(0, h - 24, w, h, fill=PANEL, outline='')
        else:
            c.create_text(w // 2, h // 2 - 20, text=_('Choose your game folder (Game & effects)',
                                                       'Выберите папку игры («Игра и эффекты»)'),
                          fill=MUTED, font=('Segoe UI', 11))
        if not self.banner_image:
            c.create_text(30, h - 70, text='Bloodborne', anchor='w', fill='#f2ead9', font=('Georgia', 36))
        info = game_info(self.var('game_dir', 'app').get())
        sub = (_('CUSA03173 · game version {}', 'CUSA03173 · версия игры {}').format(info[1]) if info
               else _('Game folder not set', 'Папка игры не выбрана'))
        c.create_text(33, h - 30, text=sub, anchor='w', fill=GOLD, font=('Segoe UI', 11))

    def scaled_art(self, path, width):
        """The cover art scaled to `width`: Pillow (smooth) or Tk's integer subsampling."""
        try:
            from PIL import Image, ImageTk
            with Image.open(path) as image:
                height = round(image.height * width / image.width)
                return ImageTk.PhotoImage(image.convert('RGB').resize((width, height), Image.LANCZOS))
        except (ImportError, OSError):
            factor = max(1, round(self.banner_image.width() / max(width, 1)))
            return self.banner_image.subsample(factor) if factor > 1 else self.banner_image

    def build_graphics(self):
        ttk = self.ttk
        f = self.scrolled_page('graphics', _('Graphics', 'Графика'),
                               _('Stored in bbport.ini; the in-game menu (Insert or L3+R3) changes the same values.',
                                 'Хранится в bbport.ini; в игре меняется через меню (Insert или L3+R3).'))
        self.section(f, _('Upscaling', 'Апскейлинг'), top=4)
        self.row(f, _('Upscaler', 'Апскейлер'), self.choice(f, 'upscaler', 'ini', UPSCALERS),
                 _("Temporal upscaling with the game's own motion vectors. FSR 4 needs its assets (below) and "
                   'a GPU with INT8 dot products; otherwise the game falls back to FSR 3.1 by itself.',
                   'Временной апскейлинг с векторами движения игры. FSR 4 нужны ассеты (ниже) и GPU с INT8; '
                   'иначе игра сама переключится на FSR 3.1.'))
        self.row(f, _('Quality preset', 'Пресет'), self.choice(f, 'preset', 'ini', PRESETS),
                 _('Render scale per axis: Quality renders at 1/1.5 of the output size.',
                   'Масштаб рендера по каждой оси: Quality рисует в 1/1.5 размера вывода.'))
        self.row(f, _('Output resolution', 'Разрешение вывода'), self.choice(f, 'output_res', 'ini', OUTPUTS),
                 _('What the upscaler produces; the HUD is drawn at this size too.',
                   'Что выдаёт апскейлер; интерфейс рисуется в этом же размере.'))
        self.row(f, _('Live resolution changes', 'Смена разрешения на лету'), self.choice(f, 'live_resolution', 'ini', LIVE),
                 _('Off: outputs other than 1080p are set by a patch at start (fastest; changing them in the '
                   'game restarts it). On: change output and preset in the game without a restart, at a cost.',
                   'Выкл.: разрешения кроме 1080p задаются патчем при запуске (быстрее). Вкл.: менять в игре '
                   'без перезапуска, но медленнее.'))
        self.check(f, 'sharpen', 'ini', _('Sharpening (RCAS)', 'Резкость (RCAS)'))
        holder = ttk.Frame(f)
        ttk.Scale(holder, from_=0.0, to=2.0, variable=self.var('sharpness', 'ini'), length=300).pack(side='left')
        value = ttk.Label(holder, width=5)
        value.pack(side='left', padx=10)
        show = lambda *_a: value.configure(text=f'{self.vars["sharpness"].get():.2f}')
        self.vars['sharpness'].trace_add('write', show)
        show()
        self.row(f, _('Sharpness', 'Сила резкости'), holder)
        self.check(f, 'object_motion', 'ini', _('Object motion vectors', 'Векторы движения объектов'),
                   _('Less ghosting on characters, cloth and weapons; costs about 10% FPS.',
                     'Меньше гостинга на персонажах и одежде; стоит около 10% FPS.'))
        self.section(f, _('FSR 4 assets', 'Ассеты FSR 4'))
        self.fsr4_label = ttk.Label(f, text='', wraplength=self.px(640), justify='left')
        self.fsr4_label.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w')
        holder = ttk.Frame(f)
        holder.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(8, 0))
        self.fsr4_button = ttk.Button(holder, text=_('Download FSR 4 assets', 'Скачать ассеты FSR 4'),
                                      command=self.download_fsr4)
        self.fsr4_button.pack(side='left')
        self.fsr4_progress = ttk.Progressbar(holder, length=280, maximum=len(fsr4_files()))
        self.fsr4_progress.pack(side='left', padx=12)
        self.note(f, _("From FireBurn/Q2RTX on GitHub (built from AMD's MIT-licensed FidelityFX source), "
                       'about 30 MB, into the fsr4_shaders folder of the port.',
                       'С GitHub FireBurn/Q2RTX (собраны из MIT-исходников AMD FidelityFX), около 30 МБ, '
                       'в папку fsr4_shaders порта.'), top=6)
        self.section(f, _('Detail', 'Детализация'))
        self.row(f, _('Model detail (LOD)', 'Детализация моделей'), self.choice(f, 'model_lod', 'ini', LODS),
                 _('A game patch (game version 1.09).', 'Патч игры (версия 1.09).'))
        self.check(f, 'show_fps', 'ini', _('Show the FPS counter', 'Показывать FPS'))

    def build_display(self):
        ttk = self.ttk
        f = self.scrolled_page('display', _('Display & FPS', 'Экран и FPS'),
                               _('Applied when the game starts.', 'Применяется при запуске игры.'))
        self.version_warning(f)
        self.section(f, _('Frame rate', 'Частота кадров'), top=4)
        self.row(f, _('Frame rate', 'Режим'), self.choice(f, 'fps_mode', 'app', FPS_MODES),
                 _("Community patches for game version 1.09. Unlocked makes the game use the real frame time. "
                   'Other game versions always run at 30 FPS (the patches would corrupt them).',
                   'Патчи сообщества для версии 1.09. «Без ограничения» — игра использует реальное время кадра. '
                   'Другие версии всегда работают в 30 FPS.'))
        self.row(f, _('Frame cap (unlocked mode)', 'Ограничение FPS (режим без ограничения)'),
                 self.choice(f, 'frame_cap', 'app', FRAME_CAPS),
                 _("Above about 120 FPS the game's movement timing breaks (running and rolling get slower, "
                   'physics and animations can glitch): higher caps are at your own risk.',
                   'Выше ~120 FPS ломается тайминг движения игры (бег и перекаты замедляются, возможны '
                   'сбои физики и анимаций): более высокие значения — на ваш риск.'))
        self.row(f, _('Frames ahead of the GPU', 'Кадров впереди GPU'), self.choice(f, 'frames_ahead', 'app', FRAMES_AHEAD),
                 _('1 keeps frame pacing even; more can raise FPS when the graphics card is the limit.',
                   '1 — ровная подача кадров; больше может поднять FPS, если упирается в видеокарту.'))
        self.section(f, _('Window', 'Окно'))
        self.check(f, 'fullscreen', 'app', _('Fullscreen', 'Полноэкранный режим'))
        self.row(f, _('Presentation', 'Режим показа кадров'), self.choice(f, 'present_mode', 'app', PRESENT_MODES))
        self.check(f, 'hdr', 'app', _('Allow HDR output', 'Разрешить HDR'),
                   _('When HDR is on in Windows and the display supports it.',
                     'Если HDR включён в Windows и монитор его поддерживает.'))

    def build_game(self):
        f = self.scrolled_page('game', _('Game & effects', 'Игра и эффекты'),
                               _('Your game dump, saves and the game patches.', 'Дамп игры, сохранения и патчи игры.'))
        self.section(f, _('Game', 'Игра'), top=4)
        self.folder(f, 'game_dir', _('Game folder', 'Папка игры'),
                    _('Choose the folder with eboot.bin', 'Выберите папку с eboot.bin'),
                    _('Your own dump of CUSA03173 (eboot.bin, sce_module, sce_sys, dvdroot_ps4); version 1.09 '
                      'for the community patches.', 'Ваш дамп CUSA03173 (eboot.bin, sce_module, sce_sys, '
                      'dvdroot_ps4); версия 1.09 для патчей сообщества.'), on_change=self.game_changed)
        self.folder(f, 'user_dir', _('Saves folder', 'Папка сохранений'),
                    _('Choose the saves folder', 'Выберите папку сохранений'),
                    _('Empty: {} (shader caches are kept there too).',
                      'Пусто: {} (там же кэш шейдеров).').format(DATA_DIR / 'user'), on_change=self.refresh_status)
        self.row(f, _('Text language', '\u042f\u0437\u044b\u043a \u0442\u0435\u043a\u0441\u0442\u0430'), self.choice(f, 'language', 'app', LANGUAGES))
        self.row(f, _('Player name', 'Имя игрока'), self.ttk.Entry(f, textvariable=self.var('player_name', 'app'), width=30),
                 _('Where the game shows the PSN name; empty: the default.', 'Где игра показывает имя PSN; пусто — по умолчанию.'))
        self.section(f, _('Effects', 'Эффекты'))
        self.version_warning(f)
        for key, title, _on in EFFECTS:
            self.check(f, key, 'ini', _(*title))
        self.section(f, _('Extras', 'Дополнительно'))
        for key, title, _on in EXTRAS:
            self.check(f, key, 'ini', _(*title))
        self.note(f, _('Effects and extras are game patches for version 1.09, applied at start.',
                       'Эффекты и дополнения — патчи игры для версии 1.09, применяются при запуске.'))

    def build_online(self):
        ttk = self.ttk
        f = self.scrolled_page('online', _('Online co-op'),
                               _('Host or join your friends over Tailscale, or play on another server.'))
        self.check(f, 'online', 'app', _('Play online'),
                   _('Off: the game stays offline, as before.'))
        self.var('online', 'app').trace_add('write', lambda *_a: self.refresh_status())
        # Each pool shows only its own settings (show_pool below). bbcoop: Host and Join show neither.
        groups = {}
        before = set(f.grid_slaves())

        def group(name):
            nonlocal before
            now = set(f.grid_slaves())
            groups[name] = groups.get(name, []) + list(now - before)
            before = now

        group('always')
        # bbcoop: Host and Join (launcher/bbcoop_online.py; docs/bbcoop/brief.md, "Easy online").
        self.section(f, _('Play together'))
        self.row(f, _('Hunter name'), ttk.Entry(f, textvariable=self.var('online_npid', 'app'), width=24),
                 _('Your name on every host: 3 to 16 letters, digits, - or _.'))
        mode = self.var('coop_mode', 'app')
        modes = ttk.Frame(f)
        for value, text in (('host', _('Host a game')), ('join', _('Join a game')), ('manual', _('Other server'))):
            ttk.Radiobutton(modes, text=text, variable=mode, value=value,
                            command=lambda: self.var('online', 'app').set(True)).pack(side='left', padx=(0, 16))
        self.row(f, _('Co-op'), modes,
                 _('Host: this computer runs the server while the launcher is open. Join: a friend who '
                   'hosts. Both of you need Tailscale on. Other server: the addresses below.'))
        group('always')
        self.host_label = ttk.Label(f, text='', justify='left', wraplength=self.px(560))
        self.row(f, _('Your server'), self.host_label)
        group('host')
        hosts = ttk.Frame(f)
        self.host_list = self.tk.Listbox(hosts, height=4, width=46, bg=CARD, fg=TEXT, selectbackground=BLOOD,
                                         highlightthickness=0, borderwidth=0, activestyle='none')
        self.host_list.pack(side='left')
        self.host_list.bind('<<ListboxSelect>>', self.on_host_picked)
        ttk.Button(hosts, text=_('Refresh'), command=self.find_hosts).pack(side='left', anchor='n', padx=(8, 0))
        self.row(f, _('Hosts'), hosts, _('Friends hosting on your Tailscale network.'))
        self.row(f, _('Host address'), ttk.Entry(f, textvariable=self.var('coop_join', 'app'), width=24),
                 _("Or type the host's Tailscale address (100.x.y.z)."))
        group('join')
        self.section(f, _('Co-op pool'))
        pool = self.var('online_pool', 'app')
        pool.trace_add('write', lambda *_a: self.refresh_status())
        pools = ttk.Frame(f)
        ttk.Radiobutton(pools, text=_('Seamless Hunt (BBSC)'), variable=pool, value='seamless').pack(side='left')
        ttk.Radiobutton(pools, text=_("The Hunter's Dream (with bbhost players)"), variable=pool,
                        value='huntersdream').pack(side='left', padx=(16, 0))
        if pool.get() not in ('seamless', 'huntersdream'):
            pool.set('seamless')
        self.row(f, _('Play with'), pools,
                 _("Seamless Hunt: BBSC's seamless co-op with bbcoop's rules, through a shadNet server "
                   "(Seamless Hunt's or a group's own) and its account."))
        group('pool')

        self.section(f, _("The Hunter's Dream account"))
        account = ttk.Label(f, text='', justify='left')
        user_dir = self.var('user_dir', 'app')

        thd_id, thd_token = self.var('thd_online_id', 'app'), self.var('thd_token', 'app')

        def show_accounts():
            name = thd_id.get() if thd_token.get() else ''
            account.configure(text=_('Signed in as {}').format(name) if name else _('Not signed in'))
        show_accounts()
        self.row(f, _('Account'), account)
        signing = ttk.Label(f, text='', style='Muted.TLabel', justify='left', wraplength=self.px(560))
        # The code to type on the website, large, with a button that copies it.
        code_box = ttk.Frame(f)
        code_label = ttk.Label(code_box, text='', style='Code.TLabel')
        code_label.pack(side='left')

        def copy_code():
            self.root.clipboard_clear()
            self.root.clipboard_append(code_label.cget('text'))
        ttk.Button(code_box, text=_('Copy'), command=copy_code).pack(side='left', padx=(14, 0))

        def show_code(code=''):
            code_label.configure(text=code)
            if code:
                code_box.grid()
            else:
                code_box.grid_remove()

        def sign_in():
            signing.configure(text=_('Starting the sign-in…'))
            show_code()
            def on_code(page, code):
                text = _('In the page that opened ({0}), sign in with Discord and enter this code:').format(page)
                self.ui_calls.put(lambda: (signing.configure(text=text), show_code(code)))
                webbrowser.open(page)

            def work():
                try:
                    name, token = online.thd_sign_in('', on_code, f'bbcoop on {os.environ.get("COMPUTERNAME", "PC")}')
                except (OSError, ValueError) as error:
                    self.ui_calls.put(lambda e=error: (signing.configure(text=_('Sign-in failed: {}').format(e)),
                                                       show_code()))
                    return
                self.ui_calls.put(lambda: (thd_id.set(name), thd_token.set(token), signing.configure(text=''),
                                           show_code(), show_accounts(), self.collect()))
            threading.Thread(target=work, daemon=True).start()
        self.row(f, '', ttk.Button(f, text=_('Sign in with Discord'), command=sign_in))
        signing.grid(row=self.next_row(f), column=1, sticky='w', pady=(6, 0))
        code_box.grid(row=self.next_row(f), column=1, sticky='w', pady=(4, 0))
        credit = ttk.Label(f, text=_("The connection to The Hunter's Dream is Awaken's port of bbhost's network "
                                     "code; bbhost and The Hunter's Dream are by droogie."),
                           style='Muted.TLabel', cursor='hand2')
        credit.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(8, 0))
        credit.bind('<Button-1>', lambda _e: webbrowser.open('https://github.com/Awakenchan/BloodBorneButPc'))
        group('huntersdream')
        self.section(f, _('Game server (messages, bloodstains, ghosts)'))
        community = self.var('online_community', 'app')
        self.row(f, _('Game server'), ttk.Entry(f, textvariable=community, width=40),
                 _('Seamless Hunt by default. Only the name is needed; the game picks the ports.'))
        self.section(f, _('Co-op server (bells and summons)'))
        server = self.var('online_server', 'app')
        self.row(f, _('Server address'), ttk.Entry(f, textvariable=server, width=40),
                 _('play.seamlesshunt.com:31313 is Seamless Hunt (BBSC). For a private server, enter '
                   'host:port from the person who runs it.'))
        self.row(f, _('WebAPI address'), ttk.Entry(f, textvariable=self.var('online_webapi', 'app'), width=40),
                 _('Of the shadNet server, not the game server. Leave empty: the server address with '
                   'port 31315 is used.'))
        self.section(f, _('Account'))
        self.row(f, _('Online ID (NPID)'), ttk.Entry(f, textvariable=self.var('online_npid', 'app'), width=40),
                 _('Your shadNet account name, not your email. Register one at seamlesshunt.com, or ask the '
                   'owner of a private server.'))
        self.row(f, _('Password'), ttk.Entry(f, textvariable=self.var('online_password', 'app'), width=40, show='•'),
                 _('Saved on this PC with the other launcher settings.'))
        self.check(f, 'online_bbsc', 'app', _('Seamless co-op (BBSC)'),
                   _("Sessions survive deaths, lamps and bosses, with bbcoop's co-op rules. Everyone in a "
                     'session needs it on.'))
        group('seamless')
        self.section(f, _('Connection'))
        self.check(f, 'online_upnp', 'app', _('UPnP (open a port on the router automatically)'),
                   _('Turn it off on Tailscale or another VPN.'))
        group('upnp')
        result = ttk.Label(f, text='', style='Muted.TLabel', justify='left')

        def check():
            values = {key: self.var(key, 'app').get()
                      for key in ('online_server', 'online_webapi', 'online_community')}
            address = values['online_server'].strip() or online.SHADNET_SERVER
            game = online.community_address(values['online_community'])
            webapi = online.webapi_setting(values['online_webapi'], address, game)
            online_on = self.var('online', 'app').get()
            hunters_dream = pool.get() == 'huntersdream'
            signed_in = thd_id.get() if thd_token.get() else ''
            result.configure(text=_('Checking…'))

            def work():
                results = online.check_thd('') if hunters_dream else online.check(address, webapi, game)
                text = '\n'.join(f'{name}: {where}: ' + (_('reachable') if ok else _('not reachable') + f' ({detail})')
                                 for name, where, ok, detail in results)
                if hunters_dream:
                    text += '\n' + (_('Signed in as {}').format(signed_in) if signed_in else
                                    _('Not signed in: use "Sign in with Discord" above.'))
                if not online_on:
                    text += '\n' + _('"Play online" is off: the game starts offline.')
                self.ui_calls.put(lambda: result.configure(text=text))
            threading.Thread(target=work, daemon=True).start()
        self.row(f, '', ttk.Button(f, text=_('Test connection'), command=check))
        result.grid(row=self.next_row(f), column=1, sticky='w', pady=(6, 0))
        group('test')
        self.note(f, _('Everyone in a session needs the same game version (1.09) and the same server. '
                       'bbcoop on Linux, macOS and Windows play together.'))
        show_code()  # hidden until a sign-in shows one; its row stays reserved (rows are counted above)
        shown = {'seamless': {'seamless', 'upnp'}, 'huntersdream': {'huntersdream', 'upnp'}}

        def show_pool(*_args):
            coop = mode.get() if mode.get() in ('host', 'join') else ''  # bbcoop
            want = {'always', coop} if coop else {'always', 'pool', 'test'} | shown.get(pool.get(), shown['seamless'])
            for name, widgets in groups.items():
                for widget in widgets:
                    if widget is not code_box:
                        widget.grid() if name in want else widget.grid_remove()
            if 'huntersdream' in want:
                show_code(code_label.cget('text'))
            else:
                code_box.grid_remove()
        pool.trace_add('write', show_pool)
        mode.trace_add('write', show_pool)
        for key in ('coop_mode', 'online'):
            self.var(key, 'app').trace_add('write', self.on_coop_mode)
        show_pool()

    # ---- bbcoop: Host and Join -----------------------------------------------------------------
    def first_start(self):
        """A new install asks for its game folder and a hunter name, nothing else; the hunter's
        password is made here (docs/bbcoop/brief.md, "Easy online")."""
        game = self.var('game_dir', 'app')
        if not game_info(game.get()):
            self.messagebox.showinfo('Bloodborne', _('Choose your Bloodborne folder: your own dump of v1.09 '
                                                     '(CUSA03173 or CUSA00900), the folder with eboot.bin in it.'))
            chosen = self.filedialog.askdirectory(title=_('Bloodborne folder'))
            if chosen:
                game.set(str(Path(chosen)))
                self.game_changed()
        name = self.var('online_npid', 'app')
        if not name.get().strip():
            from tkinter import simpledialog
            while True:
                answer = simpledialog.askstring(
                    'Bloodborne', _('Your hunter name, as your friends will see it\n(3 to 16 letters, digits, - or _):'),
                    parent=self.root)
                if answer is None:
                    break
                if bbcoop_online.valid_name(answer.strip()):
                    name.set(answer.strip())
                    self.var('coop_mode', 'app').set('join')
                    self.var('online', 'app').set(True)
                    break
                self.messagebox.showwarning('Bloodborne', _('3 to 16 letters, digits, - or _, please.'))
        if name.get().strip() and not self.var('online_password', 'app').get():
            self.var('online_password', 'app').set(bbcoop_online.new_password())
        self.collect()
        self.on_coop_mode()

    def on_coop_mode(self, *_args):
        if not getattr(self, 'coop_pending', False):  # one update for the several variables a click sets
            self.coop_pending = True
            self.root.after_idle(self.apply_coop_mode)

    def apply_coop_mode(self):
        """Hosting runs while Host is picked (and Play online is on); Join looks for hosts."""
        self.coop_pending = False
        mode, playing_online = self.var('coop_mode', 'app').get(), self.var('online', 'app').get()
        if mode == 'host' and playing_online:
            self.start_hosting()
        elif not self.process:  # a hosted game keeps its server until it ends
            self.stop_hosting()
        if mode == 'join' and playing_online:
            self.find_hosts()
        self.refresh_status()

    def ensure_host(self, name):
        """The server on this computer's address (helper threads); returns the address. Between
        games it follows that address: started before Tailscale connected, it moves there."""
        with self.host_lock:
            address, self.host_note = bbcoop_online.my_address()
            if not self.host.running() or (address != self.host.address and not self.process):
                self.host.start(address, name, self.build_id)
            return self.host.address

    def watch_host(self):
        """Every 10 s: a server whose address is no longer this computer's moves (ensure_host)."""
        self.root.after(10000, self.watch_host)
        if self.host.running() and not self.process and not self.host_lock.locked():
            threading.Thread(target=lambda: bbcoop_online.my_address()[0] != self.host.address and
                             self.ui_calls.put(self.start_hosting), daemon=True).start()

    def start_hosting(self):
        name = self.var('online_npid', 'app').get().strip() or 'Hunter'
        self.host_label.configure(text=_('Connecting Tailscale and starting the server…'))

        def work():
            problem = bbcoop_online.connect_tailscale()
            try:
                address = self.ensure_host(name)
                text = _('Your address: {0}. Friends pick "{1} — hosting" in Join, or type this '
                         'address.').format(address, name) + (f'\n{problem or self.host_note}' if problem or self.host_note else '')
            except RuntimeError as error:
                text = _('The server did not start: {}').format(error)
            self.ui_calls.put(lambda: self.host_label.configure(text=text))
        threading.Thread(target=work, daemon=True).start()

    def stop_hosting(self):
        self.host_label.configure(text='')

        def work():
            with self.host_lock:
                self.host.stop()
        threading.Thread(target=work, daemon=True).start()

    def find_hosts(self):
        self.host_list.delete(0, 'end')
        self.host_list.insert('end', _('Looking for hosts on Tailscale…'))

        def work():
            problem = bbcoop_online.connect_tailscale()
            hosts = bbcoop_online.find_hosts()
            self.ui_calls.put(lambda: self.show_hosts(hosts, problem))
        threading.Thread(target=work, daemon=True).start()

    def show_hosts(self, hosts, problem=''):
        self.host_list.delete(0, 'end')
        self.hosts_found = hosts or []
        for address, name, build in self.hosts_found:
            other = bbcoop_online.build_problem(self.build_id, build, name)
            self.host_list.insert('end', f'{name} — hosting' + (_('  (another build)') if other else ''))
            if address == self.var('coop_join', 'app').get().strip():
                self.host_list.selection_set('end')
        if hosts is None:
            self.host_list.insert('end', problem or _('Tailscale is not connected: open Tailscale and connect.'))
        elif not hosts:
            self.host_list.insert('end', _('Nobody is hosting right now. Refresh, or type an address.'))

    def on_host_picked(self, _event):
        picked = self.host_list.curselection()
        if picked and picked[0] < len(self.hosts_found):
            self.var('coop_join', 'app').set(self.hosts_found[picked[0]][0])

    def prepare_coop(self, mode):
        """Before a Host or Join game (helper thread): the server, the build check, the account."""
        name = self.var('online_npid', 'app').get().strip()
        password = self.var('online_password', 'app')
        address = self.var('coop_join', 'app').get().strip()
        if not bbcoop_online.valid_name(name):
            self.messagebox.showwarning('Bloodborne', _('Hunter name: 3 to 16 letters, digits, - or _.'))
            self.show('online')
            return
        if not password.get():
            password.set(bbcoop_online.new_password())
        secret = password.get()
        self.play_button.configure(state='disabled')
        self.status.configure(text=_('Starting your server…') if mode == 'host' else _('Checking the host…'), fg=GOLD)

        def work():
            try:
                problem = bbcoop_online.connect_tailscale()
                if problem:
                    raise ValueError(problem)
                if mode == 'host':
                    where = self.ensure_host(name)
                    bbcoop_online.make_account(where, name, secret)
                else:
                    where = address
                    bbcoop_online.prepare_join(where, name, secret, self.build_id)
            except (RuntimeError, ValueError) as error:
                self.ui_calls.put(lambda e=error: self.coop_failed(str(e)))
                return
            self.ui_calls.put(lambda: self.coop_ready(where))
        threading.Thread(target=work, daemon=True).start()

    def coop_ready(self, address):
        """The host's three server fields (bbcoop_online.server_fields), BBSC on, no UPnP; then the game."""
        for key, value in bbcoop_online.server_fields(address).items():
            self.var(key, 'app').set(value)
        self.var('online_pool', 'app').set('seamless')
        self.var('online_bbsc', 'app').set(True)
        self.var('online_upnp', 'app').set(False)
        self.collect()
        self.launch()

    def coop_failed(self, text):
        self.refresh_status()
        self.messagebox.showerror('Bloodborne', text)
        self.show('online')

    def build_mods(self):
        f = self.scrolled_page('mods', _('Mods & patches', 'Моды и патчи'),
                               _('The game files are never changed: mods are layered over them at start.',
                                 'Файлы игры не меняются: моды накладываются при запуске.'))
        self.section(f, _('Mods', 'Моды'), top=4)
        self.check(f, 'mods_enabled', 'app', _('Load mods', 'Загружать моды'),
                   _('Loose-file mods, each in its own folder (with dvdroot_ps4, or chr\\, parts\\ … directly).',
                     'Моды из файлов, каждый в своей папке (с dvdroot_ps4 или chr\\, parts\\ … напрямую).'))
        self.folder(f, 'mods_dir', _('Mods folder', 'Папка модов'), _('Choose the mods folder', 'Выберите папку модов'),
                    _('Empty: {}', 'Пусто: {}').format(DATA_DIR / 'mods'), on_change=self.refresh_lists)
        self.mods_frame = self.ttk.Frame(f)
        self.mods_frame.grid(row=self.next_row(f), column=0, columnspan=2, sticky='we', pady=(8, 0))
        self.section(f, _('Third-party patches', 'Сторонние патчи'))
        self.folder(f, 'patches_dir', _('Patches folder', 'Папка патчей'), _('Choose the patches folder', 'Выберите папку патчей'),
                    _('shadPS4/GoldHEN XML patch files for version 1.09. Empty: {}',
                      'XML-патчи shadPS4/GoldHEN для версии 1.09. Пусто: {}').format(DATA_DIR / 'patches'),
                    on_change=self.refresh_lists)
        self.patches_frame = self.ttk.Frame(f)
        self.patches_frame.grid(row=self.next_row(f), column=0, columnspan=2, sticky='we', pady=(8, 0))
        self.ttk.Button(f, text=_('Refresh', 'Обновить'), command=self.refresh_lists).grid(
            row=self.next_row(f), column=0, sticky='w', pady=(14, 0))

    def build_advanced(self):
        ttk = self.ttk
        f = self.scrolled_page('advanced', _('Advanced', 'Дополнительно'),
                               _('Launcher options, performance switches and diagnostics.',
                                 'Настройки лаунчера, производительность и диагностика.'))
        self.section(f, _('Launcher', 'Лаунчер'), top=4)
        self.row(f, _('Launcher language', 'Язык лаунчера'), self.choice(f, 'ui_language', 'app', UI_LANGUAGES),
                 _('Applies when the launcher opens again.', 'Применится при следующем открытии лаунчера.'))
        self.check(f, 'close_on_play', 'app', _('Close the launcher when the game starts', 'Закрывать лаунчер при запуске игры'))
        holder = ttk.Frame(f)
        holder.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(10, 0))
        if not MAC:  # bbcoop: PowerShell and Steam's Windows shortcuts
            ttk.Button(holder, text=_('Desktop shortcut', 'Ярлык на рабочем столе'), command=self.shortcut).pack(side='left')
            ttk.Button(holder, text=_('Add to Steam'), command=self.add_to_steam).pack(side='left', padx=(6, 0))
        ttk.Button(holder, text=_('Port folder', 'Папка порта'), command=lambda: self.open_path(DATA_DIR)).pack(side='left', padx=6)
        ttk.Button(holder, text='bbport.ini', command=lambda: self.open_path(ini_path())).pack(side='left')
        holder = ttk.Frame(f)
        holder.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(10, 0))
        ttk.Button(holder, text=_('Clear shader cache', 'Очистить кэш шейдеров'), command=self.clear_cache).pack(side='left')
        ttk.Label(holder, text=_('If the game only shows a black screen, this usually helps.',
                                 'Если игра показывает только чёрный экран, обычно это помогает.'),
                  style='Muted.TLabel').pack(side='left', padx=10)
        self.section(f, _('Performance', 'Производительность'))
        self.row(f, _('Two-stage GPU pipeline', 'Двухстадийный конвейер GPU'), self.choice(f, 'draw_pipe', 'app', DRAW_PIPE),
                 _('20–30% faster; switch it off if the game is unstable.', 'Быстрее на 20–30%; при нестабильности выключите.'))
        self.row(f, _('GPU readbacks', 'Чтение данных GPU'), self.choice(f, 'readbacks', 'app', READBACKS),
                 _('How exactly data the GPU writes is copied back for the game.',
                   'Насколько точно данные, записанные GPU, возвращаются игре.'))
        self.section(f, _('Diagnostics', 'Для разработчика'))
        self.check(f, 'frame_stats', 'app', _('Frame statistics in the log (every 5 s)', 'Статистика кадров в журнале (раз в 5 с)'))
        self.check(f, 'gpu_profile', 'app', _('GPU time per pass in the log', 'Профиль GPU в журнале'))
        self.check(f, 'vk_validation', 'app', _('Vulkan validation layers (needs the Vulkan SDK; much slower)',
                                                'Слои валидации Vulkan (нужен Vulkan SDK; сильно замедляет)'))
        self.row(f, _('Extra variables', 'Доп. переменные'), ttk.Entry(f, textvariable=self.var('extra_env', 'app'), width=58),
                 _('NAME=value pairs separated by spaces (README lists them).', 'Пары ИМЯ=значение через пробел (список в README).'))
        self.note(f, _('This launcher is Supermedo\'s Windows launcher of bbport (github.com/Supermedo/bloodborne_pc), '
                       'trimmed for bbcoop.'))

    def build_log(self):
        tk, ttk = self.tk, self.ttk
        page = ttk.Frame(self.content, padding=(20, 14, 20, 8))
        self.pages['log'] = page
        top = ttk.Frame(page)
        top.pack(fill='x', pady=(0, 8))
        ttk.Label(top, text=_('Log', 'Журнал'), font=('Georgia', 20)).pack(side='left')
        ttk.Button(top, text=_('Copy', 'Копировать'), command=self.copy_log).pack(side='right')
        ttk.Button(top, text=_('Clear', 'Очистить'), command=lambda: self.set_log('')).pack(side='right', padx=6)
        self.log = tk.Text(page, wrap='none', bg='#0a0908', fg='#cfc6b8', insertbackground=TEXT, relief='flat',
                           font=('Consolas', 9), padx=8, pady=6, state='disabled', highlightthickness=0)
        bar = ttk.Scrollbar(page, command=self.log.yview)
        self.log.configure(yscrollcommand=bar.set)
        bar.pack(side='right', fill='y')
        self.log.pack(fill='both', expand=True)

    def version_warning(self, parent):
        """A banner shown while the selected game is not version 1.09 (refresh_status)."""
        label = self.ttk.Label(parent, style='Warning.TLabel', wraplength=self.px(640), justify='left', text=_(
            'Your game is version {}: these options are patches for 1.09 and are not applied; the game runs '
            'at 30 FPS. Update the dump to 1.09 to use them.',
            'Ваша игра версии {}: эти настройки — патчи для 1.09 и не применяются; игра работает в 30 FPS. '
            'Обновите дамп до 1.09, чтобы их использовать.'))
        label.grid(row=self.next_row(parent), column=0, columnspan=2, sticky='we', pady=(6, 4))
        label.template = label.cget('text')
        self.warnings = getattr(self, 'warnings', []) + [label]

    # ---- state -------------------------------------------------------------------------------
    def game_changed(self):
        self.banner_source = None
        self.draw_banner()
        self.set_icon()
        self.refresh_status()

    def refresh_status(self):
        info = game_info(self.var('game_dir', 'app').get())
        if not info:
            game = _('✗ No eboot.bin in the game folder (Game & effects)', '✗ В папке игры нет eboot.bin («Игра и эффекты»)')
        elif info[1] == PATCH_VERSION:
            game = _('✓ Game version {}: every patch available', '✓ Версия игры {}: доступны все патчи').format(info[1])
        else:
            game = _('⚠ Game version {}: runs at 30 FPS without the community patches (they are for 1.09)',
                     '⚠ Версия игры {}: 30 FPS без патчей сообщества (они для 1.09)').format(info[1])
        user = Path(self.var('user_dir', 'app').get() or DATA_DIR / 'user')
        saves = list((user / 'savedata').glob('*/*/SPRJ*')) if (user / 'savedata').is_dir() else []
        save = (_('✓ Saves found in {}', '✓ Найдены сохранения в {}').format(user) if saves
                else _('• No saves yet: the game creates them in {}', '• Сохранений пока нет: игра создаст их в {}').format(user))
        missing = fsr4_missing()
        fsr4 = (_('✓ FSR 4 assets installed', '✓ Ассеты FSR 4 установлены') if not missing else
                _('• FSR 4 assets missing (Graphics); FSR 3.1 is used meanwhile',
                  '• Нет ассетов FSR 4 («Графика»); пока используется FSR 3.1'))
        if MAC:  # bbcoop: FSR 4 needs what Apple's GPUs lack
            fsr4 = ''
        for key, text in (('game', game), ('saves', save), ('gpu', self.gpu_text), ('fsr4', fsr4)):
            self.checks[key].configure(text=text, foreground=MUTED if text.startswith('•') else
                                       '#d9a441' if text.startswith('⚠') else '#d36b5c' if text.startswith('✗') else TEXT)
        for label in getattr(self, 'warnings', []):
            if info and info[1] != PATCH_VERSION:
                label.configure(text=label.template.format(info[1]))
                label.grid()
            else:
                label.grid_remove()
        if not self.process:
            self.play_button.configure(state='normal' if info else 'disabled')
            self.status.configure(text=self.summary() if info else _('Choose the game folder first.',
                                                                     'Сначала выберите папку игры.'), fg=MUTED)

    def summary(self):
        fps = dict(FPS_MODES).get(self.var('fps_mode', 'app').get(), ('?',))
        info = game_info(self.var('game_dir', 'app').get())
        if info and info[1] != PATCH_VERSION:
            fps = ('30 FPS',)
        upscaler = dict(UPSCALERS).get(self.var('upscaler', 'ini').get(), ('?',))[0].split(' (')[0]
        output = self.var('output_res', 'ini').get().replace('x', ' × ')
        state = _('Online') if self.var('online', 'app').get() else _('Offline')
        if self.var('online', 'app').get() and self.var('online_pool', 'app').get() == 'huntersdream':
            state = _("Online (The Hunter's Dream)")
        mode = self.var('coop_mode', 'app').get()  # bbcoop
        if self.var('online', 'app').get() and mode in ('host', 'join'):
            join = self.var('coop_join', 'app').get().strip()
            state = _('Hosting') if mode == 'host' else _('Joining {}').format(join) if join else \
                _('Offline (no host picked)')
        return f'{_(*fps)}   ·   {upscaler}   ·   {output}   ·   {state}'

    def detect_gpu(self):
        if MAC:  # bbcoop: Apple's GPU through MoltenVK; the x86-64 game needs Rosetta 2
            rosetta = subprocess.run(['/usr/bin/arch', '-x86_64', '/usr/bin/true'], capture_output=True).returncode == 0
            self.gpu_text = (_('✓ Graphics: Apple GPU (Metal); Rosetta 2 installed') if rosetta else
                             _('✗ Rosetta 2 is missing: run  softwareupdate --install-rosetta  in Terminal'))
            self.ui_calls.put(self.refresh_status)
            return
        exe = PORT_DIR / 'bin' / 'bb-gpu-capabilities.exe'
        if not exe.is_file():
            exe = PORT_DIR / 'out' / 'bb-gpu-capabilities.exe'
        env = dict(os.environ)
        if not (exe.parent / 'SDL3.dll').is_file():
            clang64 = Path(os.environ.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64' / 'bin'
            env['PATH'] = f'{clang64}{os.pathsep}{env.get("PATH", "")}'
        text = _('• Graphics card: not checked', '• Видеокарта: не проверена')
        try:
            result = subprocess.run([str(exe), '--live-resolution'], capture_output=True, text=True, timeout=30,
                                    env=env, **SPAWN)
            names = [line[5:].split(':')[0] for line in result.stderr.splitlines() if line.startswith('GPU: ')]
            if names:
                text = _('✓ Graphics card: {}', '✓ Видеокарта: {}').format(names[0])
            elif result.returncode:
                text = _('✗ No Vulkan 1.3 graphics card found (update the driver)',
                         '✗ Не найдена видеокарта с Vulkan 1.3 (обновите драйвер)')
        except (OSError, subprocess.TimeoutExpired):
            pass
        self.gpu_text = text
        self.ui_calls.put(self.refresh_status)

    def refresh_fsr4(self):
        total, missing = len(fsr4_files()), len(fsr4_missing())
        self.fsr4_progress.configure(value=total - missing)
        self.fsr4_label.configure(
            text=_('Installed: {} files in {}.', 'Установлены: {} файлов в {}.').format(total, PORT_DIR / 'fsr4_shaders')
            if not missing else _('{} of {} files missing in {}.', 'Нет {} из {} файлов в {}.').format(
                missing, total, PORT_DIR / 'fsr4_shaders'))
        self.fsr4_button.configure(state='normal' if missing and not self.downloading else 'disabled')

    def download_fsr4(self):
        self.downloading = True
        self.fsr4_button.configure(state='disabled')
        folder = PORT_DIR / 'fsr4_shaders'

        def work():
            error = None
            try:
                folder.mkdir(parents=True, exist_ok=True)
                for name in fsr4_missing():
                    part = folder / (name + '.part')
                    with urllib.request.urlopen(f'{FSR4_BASE}/{name}', timeout=60) as response:
                        part.write_bytes(response.read())
                    part.replace(folder / name)
                    self.ui_calls.put(self.refresh_fsr4)
            except OSError as failure:
                error = failure

            def done():
                self.downloading = False
                self.refresh_fsr4()
                self.refresh_status()
                if error:
                    self.messagebox.showerror('FSR 4', _('Download failed: {}', 'Не удалось скачать: {}').format(error))
            self.ui_calls.put(done)
        threading.Thread(target=work, daemon=True).start()

    def refresh_lists(self):
        if not hasattr(self, 'patches_frame'):
            return
        tk, ttk = self.tk, self.ttk
        from mods import discover
        from patches import external_patches
        for holder in (self.mods_frame, self.patches_frame):
            for widget in holder.winfo_children():
                widget.destroy()
        root = Path(self.var('mods_dir', 'app').get() or DATA_DIR / 'mods')
        profile = load_json(DATA_DIR / 'mods.json', {})
        available = discover(root)
        if set(self.mod_order) != set(available):
            known = [n for n in profile.get('order', []) if n in available]
            self.mod_order = known + [n for n in available if n not in known]
        old = {n: v.get() for n, v in self.mod_vars.items()}
        self.mod_vars = {}
        if not available:
            ttk.Label(self.mods_frame, text=_('No mods in {} yet.', 'В {} пока нет модов.').format(root),
                      style='Muted.TLabel').pack(anchor='w')
        for index, name in enumerate(self.mod_order):
            line = ttk.Frame(self.mods_frame)
            line.pack(fill='x', pady=1)
            var = tk.BooleanVar(value=old.get(name, name not in profile.get('disabled', [])))
            self.mod_vars[name] = var
            ttk.Button(line, text='▲', width=3, command=lambda i=index: self.move_mod(i, -1)).pack(side='left')
            ttk.Button(line, text='▼', width=3, command=lambda i=index: self.move_mod(i, 1)).pack(side='left', padx=(2, 10))
            ttk.Checkbutton(line, text=f'{index + 1}.  {name}', variable=var).pack(side='left')
        if len(self.mod_order) > 1:
            ttk.Label(self.mods_frame, text=_('Lower in the list loads later and wins conflicts.',
                                              'Ниже в списке — загружается позже и перекрывает.'),
                      style='Muted.TLabel').pack(anchor='w', pady=(4, 0))
        chosen = load_json(DATA_DIR / 'patches.json', {})
        found = external_patches(Path(self.var('patches_dir', 'app').get() or DATA_DIR / 'patches'), PATCH_VERSION)
        old = {n: v.get() for n, v in self.patch_vars.items()}
        self.patch_vars = {}
        if not found:
            ttk.Label(self.patches_frame, text=_('No patch files for version 1.09 yet.', 'Пока нет патчей для версии 1.09.'),
                      style='Muted.TLabel').pack(anchor='w')
        for key, _path, meta in found:
            on = key in chosen.get('enabled', []) or (key not in chosen.get('disabled', []) and
                                                      meta.get('isEnabled', 'false').lower() == 'true')
            var = tk.BooleanVar(value=old.get(key, on))
            self.patch_vars[key] = var
            author = meta.get('Author')
            ttk.Checkbutton(self.patches_frame, text=key + (f'  —  {author}' if author else ''), variable=var).pack(anchor='w')

    def move_mod(self, index, step):
        other = index + step
        if 0 <= other < len(self.mod_order):
            self.mod_order[index], self.mod_order[other] = self.mod_order[other], self.mod_order[index]
            self.refresh_lists()

    def collect(self):
        """Writes settings.json, bbport.ini, mods.json and patches.json."""
        for key, var in self.vars.items():
            if var.store != 'ini':
                try:
                    self.app[key] = var.get()
                except self.tk.TclError:  # an unfinished number in a spinbox
                    self.app[key] = APP_DEFAULTS.get(key)
        CONFIG_DIR.mkdir(parents=True, exist_ok=True)
        CONFIG_FILE.write_text(json.dumps(self.app, indent=2, ensure_ascii=False), encoding='utf-8')
        # bbcoop: the in-game menu writes bbport.ini too (L3+R3): only what was changed here since the
        # window showed the file replaces its current value; the rest stays as the game left it.
        values = self.ini_values()
        self.ini, self.ini_lines = load_ini()
        self.ini.update({key: value for key, value in values.items() if value != self.ini_shown.get(key)})
        self.ini_shown = values
        save_ini({key: self.ini[key] for key in INI_DEFAULTS}, self.ini_lines)
        self.ini, self.ini_lines = load_ini()
        if self.mod_order:
            (DATA_DIR / 'mods.json').write_text(json.dumps(
                {'order': self.mod_order, 'disabled': [n for n, v in self.mod_vars.items() if not v.get()]},
                indent=2), encoding='utf-8')
        if self.patch_vars:
            chosen = load_json(DATA_DIR / 'patches.json', {})
            shown = set(self.patch_vars)
            enabled = {k for k in chosen.get('enabled', []) if k not in shown} | {k for k, v in self.patch_vars.items() if v.get()}
            disabled = {k for k in chosen.get('disabled', []) if k not in shown} | {k for k, v in self.patch_vars.items() if not v.get()}
            (DATA_DIR / 'patches.json').write_text(json.dumps(
                {'enabled': sorted(enabled), 'disabled': sorted(disabled)}, indent=2), encoding='utf-8')

    def ini_values(self):
        """bbcoop: the bbport.ini keys the window shows, as its variables hold them."""
        values = {}
        for key, var in self.vars.items():
            if var.store == 'ini':
                try:
                    value = var.get()
                except self.tk.TclError:  # an unfinished number in a spinbox
                    value = INI_DEFAULTS.get(key)
                values[key] = ('1' if value else '0') if key in INI_FLAGS else \
                    f'{float(value):.2f}' if key == 'sharpness' else str(value)
        return values

    def reload_ini(self):
        """bbcoop: after a game: this window's changes saved, then the file (with the in-game menu's)
        shown."""
        self.collect()
        for key, var in self.vars.items():
            if var.store == 'ini':
                value = self.ini.get(key, INI_DEFAULTS.get(key))
                var.set(value == '1' if key in INI_FLAGS else float(value or 0.5) if key == 'sharpness' else value)
        self.ini_shown = self.ini_values()

    # ---- game process ------------------------------------------------------------------------
    def play(self):
        if self.process:
            return
        playing_online, npid = self.var('online', 'app'), self.var('online_npid', 'app')
        mode = self.var('coop_mode', 'app').get()  # bbcoop: Host and Join always have a name
        if not playing_online.get() and npid.get().strip() and mode not in ('host', 'join'):
            choice = self.messagebox.askyesnocancel(
                'Bloodborne', _('An online account is filled in, but "Play online" is off.\n\n'
                                'Yes: play online.   No: play offline.'))
            if choice is None:
                return
            playing_online.set(bool(choice))
        pool_name = self.var('online_pool', 'app').get()
        hunters_dream = pool_name == 'huntersdream'
        if playing_online.get() and hunters_dream and not self.var('thd_token', 'app').get():
            self.messagebox.showwarning(
                'Bloodborne', _("To play online with The Hunter's Dream, sign in with Discord first."))
            self.show('online')
            return
        if playing_online.get() and pool_name == 'seamless' and '@' in npid.get() and mode == 'manual':
            self.messagebox.showwarning(
                'Bloodborne', _('The Online ID is the account name (NPID) you registered on the server, '
                                'not your email address.'))
            self.show('online')
            return
        self.collect()
        if not game_info(self.app['game_dir']):
            self.messagebox.showerror('Bloodborne', _('Choose the game folder with eboot.bin (CUSA03173).',
                                                      'Выберите папку игры с eboot.bin (CUSA03173).'))
            self.show('game')
            return
        if playing_online.get() and mode in ('host', 'join') and not bbcoop_online.join_without_host(self.app):
            self.prepare_coop(mode)  # then launch()
            return
        self.launch()

    def launch(self):
        self.set_log('')
        try:
            self.process = subprocess.Popen(run_command(), cwd=PORT_DIR, env=game_environment(self.app),
                                            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                            stderr=subprocess.STDOUT, **SPAWN)
        except OSError as error:
            self.append(_('Could not start: {}', 'Не удалось запустить: {}').format(error) + '\n')
            self.process = None
            return
        self.job = GameJob(self.process)
        threading.Thread(target=self.read_output, args=(self.process,), daemon=True).start()
        self.play_button.configure(state='disabled')
        self.stop_button.configure(state='normal')
        self.status.configure(text=_('Preparing the game; it opens in its own window…',
                                     'Подготовка игры; она откроется в своём окне…'), fg=GOLD)
        if self.app.get('close_on_play'):
            self.root.after(5000, lambda: self.process and self.close())

    def read_output(self, process):
        """Helper thread: the game's output to the Log page and to <saves folder>/last_run.log."""
        log = None
        try:
            log_dir = Path(self.app.get('user_dir') or DATA_DIR / 'user')
            log_dir.mkdir(parents=True, exist_ok=True)
            log = open(log_dir / 'last_run.log', 'w', encoding='utf-8', buffering=1)
        except OSError:
            pass
        warned = False
        for raw in iter(process.stdout.readline, b''):
            text = raw.decode('utf-8', errors='replace')
            self.output.put(text)
            if not warned and 'eboot.bin is not the 01.09 build' in text:
                warned = True
                self.ui_calls.put(lambda: self.messagebox.showwarning('Bloodborne', _(
                    "Your eboot.bin is not the clean 1.09 one (modified, or from another dump). The port "
                    "cannot patch it: the game runs at 30 FPS, the Game & effects options do nothing, and "
                    "some scenes (like the cutscene after Rom) can freeze.\n\nReplace eboot.bin with the one "
                    "from an unmodified 1.09 update.")))
            if log:
                log.write(text.replace('\r\n', '\n'))
        if log:
            log.close()
        self.output.put((process.wait(),))

    def drain_output(self):
        while not self.ui_calls.empty():
            self.ui_calls.get_nowait()()
        try:
            for _i in range(500):
                item = self.output.get_nowait()
                if isinstance(item, tuple):
                    self.append(_('\n— the game exited (code {}) —\n', '\n— игра завершилась (код {}) —\n').format(item[0]))
                    self.process = None
                    self.reload_ini()  # bbcoop: keep the in-game menu's changes
                    if self.job:
                        self.job.close()
                    self.stop_button.configure(state='disabled')
                    self.refresh_status()
                    if self.hidden:
                        if item[0] == 0:
                            self.root.destroy()
                            return
                        # It stopped with an error: back with its log instead of vanishing.
                        self.hidden = False
                        self.root.deiconify()
                        self.show('log')
                else:
                    if 'Entering original x86-64 code' in item:
                        self.status.configure(text=_('The game is running.', 'Игра запущена.'), fg=GOLD)
                    elif 'restarting through run.py' in item:
                        self.status.configure(text=_('Restarting with the new settings…', 'Перезапуск с новыми настройками…'), fg=GOLD)
                    self.append(item)
        except queue.Empty:
            pass
        self.root.after(100, self.drain_output)

    def stop(self):
        if self.job:
            self.job.terminate()

    def append(self, text):
        self.log.configure(state='normal')
        self.log.insert('end', text)
        lines = int(self.log.index('end-1c').split('.')[0])
        if lines > MAX_LOG_LINES:
            self.log.delete('1.0', f'{lines - MAX_LOG_LINES}.0')
        self.log.see('end')
        self.log.configure(state='disabled')

    def set_log(self, text):
        self.log.configure(state='normal')
        self.log.delete('1.0', 'end')
        self.log.insert('end', text)
        self.log.configure(state='disabled')

    def copy_log(self):
        self.root.clipboard_clear()
        self.root.clipboard_append(self.log.get('1.0', 'end'))

    def open_path(self, path, key=None):
        if key == 'user_dir' and not path:
            path = DATA_DIR / 'user'
        elif key == 'mods_dir' and not path:
            path = DATA_DIR / 'mods'
        elif key == 'patches_dir' and not path:
            path = DATA_DIR / 'patches'
        path = Path(path or DATA_DIR)
        if path.suffix == '.ini':
            self.collect()
        elif not path.exists():
            path.mkdir(parents=True, exist_ok=True)
        if MAC:  # bbcoop
            subprocess.Popen(['open', str(path)])
        else:
            os.startfile(str(path))

    # ---- updates -------------------------------------------------------------------------------

    def clear_cache(self):
        """Shader and pipeline caches; they are rebuilt while playing."""
        if self.process:
            return
        import shutil
        cache = Path(self.app['user_dir'] or DATA_DIR / 'user') / 'cache'
        shutil.rmtree(cache, ignore_errors=True)
        self.messagebox.showinfo('Bloodborne', _('Shader cache cleared. The next start stutters for a few minutes while '
                                                 'it is rebuilt.', 'Кэш шейдеров очищен. Следующий запуск несколько '
                                                 'минут будет подтормаживать, пока кэш собирается заново.'))

    def shortcut(self):
        """Bloodborne.lnk on the desktop."""
        if FROZEN:
            target, arguments, icon = sys.executable, '', f'{sys.executable},0'
        else:  # a source tree: the launcher script with the windowless Python
            pythonw = Path(sys.executable).with_name('pythonw.exe')
            target = str(pythonw if pythonw.exists() else sys.executable)
            arguments, icon = Path(__file__).resolve(), PORT_DIR / 'launcher' / 'bloodborne.ico'
        script = ('$s=(New-Object -ComObject WScript.Shell).CreateShortcut([Environment]::GetFolderPath("Desktop")'
                  '+"\\Bloodborne.lnk");'
                  f'$s.TargetPath="{target}";$s.WorkingDirectory="{PORT_DIR}";$s.IconLocation="{icon}";'
                  + (f"$s.Arguments='\"{arguments}\"';" if arguments else '') + '$s.Save()')
        result = subprocess.run(['powershell', '-NoProfile', '-Command', script], capture_output=True,
                                creationflags=NO_WINDOW)
        if result.returncode == 0:
            self.messagebox.showinfo('Bloodborne', _('Shortcut created on the desktop.', 'Ярлык создан на рабочем столе.'))
        else:
            self.messagebox.showerror('Bloodborne', result.stderr.decode(errors='replace')[:400])

    def add_to_steam(self):
        """Bloodborne in the Steam library (a non-Steam game that starts Play Bloodborne.exe)."""
        import bbport_steam
        steam = bbport_steam.steam_folder()
        configs = bbport_steam.user_configs(steam) if steam else []
        if not configs:
            self.messagebox.showerror('Bloodborne', _('Steam was not found on this PC (or nobody has signed in to it yet).'))
            return
        if FROZEN:
            exe, options = PORT_DIR / 'Play Bloodborne.exe', ''
        else:
            pythonw = Path(sys.executable).with_name('pythonw.exe')
            exe, options = (pythonw if pythonw.exists() else Path(sys.executable)), f'"{Path(__file__).resolve()}" --play'
        self.collect()  # Play Bloodborne.exe starts the game with the saved settings
        restart = False
        if bbport_steam.steam_running():
            if not self.messagebox.askyesno('Bloodborne', _('Steam has to close while the game is added; it starts '
                                                            'again afterwards. Close Steam now?')):
                return
            subprocess.Popen([str(steam / 'steam.exe'), '-shutdown'], creationflags=NO_WINDOW)
            for _i in range(60):
                time.sleep(0.5)
                if not bbport_steam.steam_running():
                    break
            else:
                self.messagebox.showerror('Bloodborne', _('Steam did not close. Close it yourself and try again.'))
                return
            time.sleep(1)  # Steam writes its files as it exits
            restart = True
        try:
            for config in configs:
                bbport_steam.add_shortcut(config / 'shortcuts.vdf', 'Bloodborne', str(exe), str(PORT_DIR),
                                          str(exe), options)
        except (OSError, ValueError) as error:
            self.messagebox.showerror('Bloodborne', _('Could not add the game to Steam: {}').format(error))
            return
        if restart:
            os.startfile('steam://open/games')
        self.messagebox.showinfo('Bloodborne', _('Bloodborne is in your Steam library. It starts with the settings '
                                                 'saved in this launcher.'))

    def close(self):
        try:
            self.collect()
        except Exception:  # never keep the window open over a settings problem
            pass
        if self.process:
            # The game's output still comes through this process (Log page, last_run.log). An
            # exited launcher closed that pipe, and the next line a preparation script or the
            # in-game restart printed failed (OSError: [Errno 22] Invalid argument): the game
            # never started. Hidden instead, it exits with the game.
            self.hidden = True
            self.root.withdraw()
            return
        self.root.destroy()


class GameJob:
    """A Windows job holding run.py and everything it starts: bb-probe.exe and the launches made
    by the in-game restart (no longer descendants of the first process). bbcoop: elsewhere the
    process group run.sh starts in (SPAWN)."""

    def __init__(self, process):
        self.process, self.handle = process, None
        if sys.platform != 'win32':
            return
        kernel32 = ctypes.windll.kernel32
        kernel32.CreateJobObjectW.restype = ctypes.c_void_p
        self.handle = kernel32.CreateJobObjectW(None, None)
        if self.handle:
            kernel32.AssignProcessToJobObject(ctypes.c_void_p(self.handle), ctypes.c_void_p(int(process._handle)))

    def terminate(self):
        if sys.platform != 'win32':
            try:
                os.killpg(self.process.pid, signal.SIGTERM)
            except OSError:
                pass
            return
        if self.handle:
            ctypes.windll.kernel32.TerminateJobObject(ctypes.c_void_p(self.handle), 1)

    def close(self):
        if self.handle:
            ctypes.windll.kernel32.CloseHandle(ctypes.c_void_p(self.handle))
            self.handle = None


def play_without_window(settings):
    """--play: the game with the saved settings (shortcuts, Steam). Output goes to the console
    when there is one and to <saves folder>/last_run.log."""
    attach_stdio()
    log_dir = Path(settings.get('user_dir') or DATA_DIR / 'user')
    log_dir.mkdir(parents=True, exist_ok=True)
    with open(log_dir / 'last_run.log', 'w', encoding='utf-8', buffering=1) as log:
        process = subprocess.Popen(run_command(), cwd=PORT_DIR, env=game_environment(settings),
                                   stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, **SPAWN)
        for raw in iter(process.stdout.readline, b''):
            text = raw.decode('utf-8', errors='replace')
            log.write(text)
            try:
                sys.stdout.write(text)
            except (OSError, ValueError):
                pass
        return process.wait()


def main():
    global LANG
    args = sys.argv[1:]
    if args and args[0] in ('--run', '--script'):
        sys.exit(run_role(args))
    settings = {**APP_DEFAULTS, **load_json(CONFIG_FILE, {})}
    LANG = settings.get('ui_language') or windows_language()
    # Without a usable game folder there is nothing to play yet: open the launcher instead.
    if '--play' in args and (Path(settings['game_dir'] or '.') / 'eboot.bin').is_file():
        sys.exit(play_without_window(settings))
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)  # sharp text on scaled displays
    except (AttributeError, OSError):
        pass
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
    root = tk.Tk()
    launcher = Launcher(root, tk, ttk, filedialog, messagebox)
    try:
        root.mainloop()
    finally:
        launcher.host.stop()  # bbcoop: hosting ends with the launcher


if __name__ == '__main__':
    main()
