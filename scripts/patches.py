"""Compile selected shadPS4/GoldHEN XML patches into out/patches.bin for the loader.

Patch addresses are PS4 virtual addresses (eboot base 0x400000); the loader's
image places eboot vaddr 0 at image offset 0. Only literal writes are supported
(bytes, bytes16/32/64, float32/64, utf8, utf16); pattern ("mask") patches are rejected.
"""
import argparse
import json
import os
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

EBOOT_BASE=0x400000
# BB_FPS presets: patch names from patches/Bloodborne.xml (app version 01.09).
# Above 60 FPS the sprint fix always goes with the frame rate patch: without it sprinting drops to
# half speed (the game's wall detector measured distance per frame, a 30 FPS rule).
FPS_PRESETS={'30':[],'60':['60 FPS++'],'90':['90 FPS++','Sprint Fix (High FPS)'],
             'uncap':['Uncap FPS++'],'120':['120 FPS++']}
# bbcoop: Uncap FPS++ and 120 FPS++ are ps4_cheats' of 2026-10-10, with yogesh1239's timestep fixes
# and the sprint fix built in. BB_FPS_PATCH=old selects the previous Uncap FPS++ (2026-08-28).
OLD_FPS_PRESETS={'uncap':['Uncap FPS++ (2026-08-28)','Sprint Fix (High FPS)']}
# Above 30 FPS the frame rate is done by hooks in the game's code (gpu/shim/bbport_frame_rate.cpp:
# 60 and 90 are the same as uncap, at most that many frames a second): no patches. The patches
# only with BB_FRAME_HOOKS=0 (comparisons).


def fps_patches(fps, env=os.environ):
    """The frame rate patches for a BB_FPS preset (BB_FPS_PATCH=old: the previous version)."""
    if fps!='30' and env.get('BB_FRAME_HOOKS','1')!='0':
        return []
    if env.get('BB_FPS_PATCH','')=='old' and fps in OLD_FPS_PRESETS:
        return list(OLD_FPS_PRESETS[fps])
    return list(FPS_PRESETS[fps])


# bbcoop: A/B switches for the current Uncap FPS++ (docs/bbcoop/animation_re.md "Foliage speed").
# BB_FPS_SKIP=<group>[,<group>] leaves out the lines that start at these addresses, so the sites keep
# the game's bytes (a cave the hook jumped to stays, unused). Each group is a set of hooks or values
# that works without the others.
FPS_SKIP_GROUPS={
    # Havok Cloth: constraint stiffness x (frame time x 60)^2 and one sub-step (hclSimulateOperator).
    'cloth':[0x00B20DEE,0x00B2B2FE,0x00B30ADE,0x00B38711,0x00B41F4E,0x00B420EE,0x00B6E67A],
    # The wind/foliage constants function 0x2713870: both steps follow the frame time (back: 1/60 a frame).
    'grass':[0x02713D76,0x02713DB1,0x02713DBB,0x02715D75],
    # SFX frame rate 30 -> 60 (frame counts / rate = seconds) and its fade-time floors.
    'sfx':[0x026FB39F,0x04CAD7FC,0x04C9F358,0x04CAD65C,0x04CAD660,0x04CAD664],
    # Other "per second at 30" values doubled.
    'fps60':[0x013EF51C,0x013EF6EC,0x0187305D,0x0187458D,0x0187AFEC,0x0187B0AD,0x01880CFD,0x01880E1A,
             0x018B254D,0x018B382D,0x01AB67EF,0x01C9F889,0x01CA975A,0x01CA9892,0x0203ACB7,0x021BB004,
             0x023DD026,0x023DEDE6,0x04D02F10,0x04D02F14,0x04D02F18,0x04D02F1C,0x04D02F70,0x04D02F74,
             0x04D02F78,0x04D02F7C],
    # hknpWorldCinfo default solver step 1/30 -> 1/120 (the old patch's line missed it).
    'hknp':[0x00F383CA],
    # Time accumulators of 0x12BBDA0's objects: real frame time instead of 1/60 a frame.
    'timeobj':[0x0267784B,0x02677892],
    # The character clip player 0x1915EC0: real frame time instead of 1/30 a frame.
    'clip':[0x01915FAB,0x01916030,0x01915F71],
}
# BB_FPS_TRY=<variant>[,<variant>] picks the bbcoop fixes (patches/Bloodborne.xml) for Uncap FPS++ (default
# DEFAULT_FPS_TRY, "none" for none); a variant's bytes replace the Uncap FPS++ lines they overlap.
FPS_TRY_VARIANTS={
    # The update context at 0x1972A7D gets the clamped frame time (0x0575A908) instead of a fixed 1/30.
    'step1972':'bbcoop: Frame Time Step at 0x1972A7D',
    # The wind offsets of 0x2713870 (a step x velocity, sent to the GPU every frame) get a fixed step
    # again (1/30: the game's; 1/60: the old patch's), its timers keep the frame time.
    'wind30':'bbcoop: Fixed Wind Offset Step 1/30',
    'wind60':'bbcoop: Fixed Wind Offset Step 1/60',
    # Havok Cloth stepped in fixed 1/120 substeps from an accumulator, with one constant stiffness factor
    # (Uncap FPS++'s factor follows every frame's time, so stiff cloth flickers).
    'clothstep':'bbcoop: Fixed Cloth Step 1/120',
    # The clamped frame time (0x02418E3D) gets a 1/240 floor: the flipper's dt can be 0 (character editor model invisible).
    'ctx30':'bbcoop: Context dt 1/30 (test)',
    'dtfloor':'bbcoop: Frame Time Floor 1/240',
}


# Applied with Uncap FPS++ unless BB_FPS_TRY says otherwise (BB_FPS_TRY=none: Uncap FPS++ as published). Asad's
# A/B 2026-10-12: wind60 calms the flowers and capes, clothstep the Doll's hood and skirt. dtfloor (2026-10-13):
# the character editor's model stayed unchanged or invisible when the frame time was 0; with the floor it updates.
DEFAULT_FPS_TRY='wind60,clothstep,dtfloor'


def fps_choices(env=os.environ):
    """(skipped line addresses, variant patch names, explicit) from BB_FPS_SKIP / BB_FPS_TRY; explicit is
    False when only the default variants apply."""
    def parse(name, table, default=''):
        value=env.get(name,default)
        keys=[] if value.strip()=='none' else [k.strip() for k in value.split(',') if k.strip()]
        unknown=[k for k in keys if k not in table]
        if unknown: raise ValueError(f'{name}: unknown {unknown}; known: {sorted(table)}')
        return keys
    skip={a for k in parse('BB_FPS_SKIP',FPS_SKIP_GROUPS) for a in FPS_SKIP_GROUPS[k]}
    variants=[FPS_TRY_VARIANTS[k] for k in parse('BB_FPS_TRY',FPS_TRY_VARIANTS,DEFAULT_FPS_TRY)]
    return skip,variants,bool(skip) or 'BB_FPS_TRY' in env


def apply_fps_choices(writes, skip, variants, replaceable=None):
    """writes without the skipped lines and without the lines a variant replaces, then the variants'.
    A variant may only replace whole lines whose offsets are in replaceable (Uncap FPS++'s), and
    variants may not overlap each other."""
    skip={a-EBOOT_BASE for a in skip}
    taken=set()
    for o,data in variants:
        span={o+i for i in range(len(data))}
        if span&taken: raise ValueError(f'BB_FPS_TRY: variants overlap at {o+EBOOT_BASE:#x}')
        taken|=span
    out=[]
    for o,data in writes:
        span={o+i for i in range(len(data))}
        if o in skip: continue
        if span&taken:
            if (replaceable is not None and o not in replaceable) or not span<=taken:
                raise ValueError(f'BB_FPS_TRY: a variant would cut the line at {o+EBOOT_BASE:#x}')
            continue
        out.append((o,data))
    return out+list(variants)
# Upscaler presets (bbport.ini "preset", the in-game menu): output / render size ratio. The game
# then renders at 1920x1080 / ratio and the port's temporal upscaler restores the output size.
OUTPUT_SIZE=(1920,1080)
PRESET_SCALES=[1.0,1.5,1.7,2.0,3.0]
# The upscaler preset without one in bbport.ini, as the GPU library's (bbport_settings.h): Native AA,
# and Quality on macOS (MetalFX).
DEFAULT_PRESET='1' if sys.platform=='darwin' else '0'
# The community patch changes two independent consumers: the game render/window setup
# and the UI movie viewport. Keep the latter at native size so glyph rasterisation and
# vector tessellation do not inherit the scene's FSR resolution.
RESOLUTION_TEMPLATE='Resolution Patch 1280x720 (16:9)'
# Effect switches (bbport.ini, in-game menu, launcher): key -> (patch when the key is 0, patch
# when it is 1). The game reads these at start; a change applies after a restart.
EFFECTS={
    'effect_chromatic_aberration':('Disable Chromatic Aberration',None),
    'effect_dof':('Disable DoF',None),
    'effect_motion_blur':('Disable Motion Blur (perf increase)',None),
    'effect_ssao':('Disable SSAO',None),
    'effect_game_aa':('Disable AA',None),
    'effect_dynamic_shadows':('Disable Dynamic Light Shadows (perf increase)',None),
    'effect_ssr':(None,'Enable Screen Space Reflections (READ NOTE)'),
    'skip_intro':(None,'Skip Intro'),
    'debug_camera':(None,'Restore Debug Camera'),
    'debug_menu':(None,'Restore Debug Menu (READ NOTES)'),
}
# Intel CPUs: the game's tone mapping turns black (DLC areas most; reported as darker than on the
# PS4): the game code runs natively, and Intel's approximate float instructions differ from the
# PS4's AMD CPU, so a block the game gates on a computed flag never runs. The community fix runs
# it always; on by default on Intel. BB_INTEL_TONEMAP_FIX=0/1 forces it off or on.
INTEL_TONEMAP='Intel Black Tonemap Fix'


def intel_cpu(cpuinfo='/proc/cpuinfo'):
    if sys.platform == 'darwin' and cpuinfo == '/proc/cpuinfo':
        return True  # bbcoop: Rosetta 2 shows the game a GenuineIntel CPU
    try:
        with open(cpuinfo) as f:
            return any(line.startswith('vendor_id') and 'GenuineIntel' in line for line in f)
    except OSError:
        return False


# The title's PLAY ONLINE / PLAY OFFLINE dialog: the port has no PSN, the game goes straight to the
# main menu offline. On by default (the launcher's switch); BB_SKIP_NETWORK_CHOICE=0 shows it.
SKIP_NETWORK_CHOICE='Skip Online/Offline Choice'


def skip_network_choice(env=os.environ):
    return env.get('BB_SKIP_NETWORK_CHOICE','1')!='0'


def intel_tonemap_fix(env=os.environ, cpuinfo='/proc/cpuinfo'):
    forced=env.get('BB_INTEL_TONEMAP_FIX')
    return forced=='1' if forced in ('0','1') else intel_cpu(cpuinfo)

# model_lod: -2 highest, 0 the game's, 1 lower, 2 lowest.
MODEL_LOD={'-2':'Model LOD -2 (Highest)','1':'Model LOD 1 (Lower)','2':'Model LOD 2 (Lowest)'}


DEBUG_MENU='Restore Debug Menu (READ NOTES)'
# What the patched game reads for the debug menu (BB_FILE_TRACE=1), under dvdroot_ps4. The game asks
# for lower-case names; the PS4's file system and the port's runtime ignore case.
DEBUG_MENU_FONTS=('adhoc/font/DbgFont14h.ccm','adhoc/font/DbgFont14h.tpf')
DEBUG_MENU_SHADERS=('adhoc/FontShader/debugFont_vs.vpo','adhoc/FontShader/debugFont_ps.ppo')


def find_ignoring_case(root, relative):
    """`root`/`relative`, each component matched ignoring case; None when absent."""
    path=Path(root)
    for part in relative.split('/'):
        exact=path/part
        if exact.exists():
            path=exact
            continue
        try:
            matches=[e for e in path.iterdir() if e.name.casefold()==part.casefold()]
        except OSError:
            return None
        if not matches:
            return None
        path=matches[0]
    return path


def debug_menu_problem(game):
    """Why the debug menu patch cannot work with this game folder, or None."""
    dvdroot=Path(game)/'dvdroot_ps4'
    missing=[]
    for relative in DEBUG_MENU_FONTS:
        found=find_ignoring_case(dvdroot,relative)
        if not found or not found.is_file() or found.stat().st_size==0:
            missing.append(relative)
    if not missing:
        return None
    hint=''
    if all(find_ignoring_case(dvdroot,'font/'+r.rsplit('/',1)[1]) for r in missing):
        hint=' (they are in dvdroot_ps4/font: the game reads them from dvdroot_ps4/adhoc/font)'
    return ('the debug menu needs the font files from https://www.nexusmods.com/bloodborne/mods/253 '
            f'in dvdroot_ps4 of the game folder or of a mod: missing {", ".join(missing)}{hint}')


def validate_patch_requirements(names, game):
    """The patches to apply: raises for conflicts; drops the debug menu (with a message) when its
    font files are missing, as the game crashes opening the menu without them."""
    if 'Restore Debug Camera' in names and 'Enemy Control' in names:
        raise ValueError('Restore Debug Camera conflicts with Enemy Control; enable only one')
    if DEBUG_MENU in names:
        problem=debug_menu_problem(game)
        if problem:
            print(f'Patches: debug menu off: {problem}',file=sys.stderr)
            return [n for n in names if n!=DEBUG_MENU]
        absent=[r for r in DEBUG_MENU_SHADERS if not find_ignoring_case(Path(game)/'dvdroot_ps4',r)]
        if absent:
            print(f'Patches: debug menu: {", ".join(absent)} not found (the mod\'s font shaders); '
                  'the menu may not draw',file=sys.stderr)
    return names


def effect_patches(settings):
    names=[]
    for key,(off,on) in EFFECTS.items():
        if key not in settings: continue
        name=on if settings[key]=='1' else off
        if name: names.append(name)
    lod=MODEL_LOD.get(settings.get('model_lod','0'))
    if lod: names.append(lod)
    return names
SCENE_WIDTH=0x02196A6B-EBOOT_BASE
SCENE_HEIGHT=0x02196A7A-EBOOT_BASE
UI_WIDTH=0x02358554-EBOOT_BASE
UI_HEIGHT=0x0235855D-EBOOT_BASE


def read_settings(path):
    settings={}
    if path.exists():
        for line in path.read_text(encoding='utf-8').splitlines():
            key,sep,value=line.partition('=')
            if sep and not line.startswith('#'): settings[key.strip()]=value.strip()
    return settings


def render_size(settings,override=''):
    """Render resolution for the upscaler preset, or None for native."""
    if override:
        w,h=(int(v) for v in override.lower().split('x'))
        return (w,h)
    if settings.get('upscaler','fsr3')=='off': return None
    preset=int(settings.get('preset',DEFAULT_PRESET) or 0)
    scale=PRESET_SCALES[max(0,min(preset,len(PRESET_SCALES)-1))]
    if scale==1.0: return None
    # Even sizes (the game has half-resolution buffers).
    return tuple(max(2,round(v/scale/2)*2) for v in OUTPUT_SIZE)


def output_size(settings):
    """Output (UI) size from bbport.ini output_res, e.g. 3840x2160; 1920x1080 by default."""
    try:
        w,h=(int(v) for v in settings.get('output_res','').lower().split('x'))
        if w>0 and h>0: return (w,h)
    except ValueError:
        pass
    return OUTPUT_SIZE


def scaled_sizes(settings):
    """(render, output) for an output other than 1080p (above it, or 720p for the Steam Deck):
    the game renders at output / preset scale (or at the output size without upscaler) and the
    upscaler fills the output. None at 1080p and for TAA (native, live host targets only)."""
    out=output_size(settings)
    if out==OUTPUT_SIZE or settings.get('upscaler')=='taa': return None
    scale=1.0
    if settings.get('upscaler','fsr3')!='off':
        preset=int(settings.get('preset',DEFAULT_PRESET) or 0)
        scale=PRESET_SCALES[max(0,min(preset,len(PRESET_SCALES)-1))]
    render=tuple(max(2,round(v/scale/2)*2) for v in out)
    # A scene of exactly 1920x1080 (4K Performance) is indistinguishable from the game's UI
    # coordinate space, which the port's UI composition recognizes by that size.
    if render==OUTPUT_SIZE: render=(1916,1078)
    return render,out


def resolution_writes(xml,size,app_version,segments,ui=OUTPUT_SIZE):
    writes=compile_patches(xml,[RESOLUTION_TEMPLATE],app_version,segments)
    replacements={SCENE_WIDTH:(0xB8,0x500,size[0]),
                  SCENE_HEIGHT:(0xB8,0x2D0,size[1]),
                  UI_WIDTH:(0xB8,0x500,ui[0]),
                  UI_HEIGHT:(0xB9,0x2D0,ui[1])}
    out=[]
    seen=set()
    for offset,data in writes:
        if offset in replacements:
            opcode,old,value=replacements[offset]
            if data!=bytes([opcode])+old.to_bytes(3,'little') or offset in seen:
                raise ValueError(f'unexpected resolution patch at {offset+EBOOT_BASE:#x}')
            data=bytes([opcode])+value.to_bytes(3,'little')
            seen.add(offset)
        out.append((offset,data))
    if seen!=replacements.keys():
        raise ValueError('resolution patch is missing scene/UI viewport instructions')
    return out


def eboot_segments(elf):
    phoff,=struct.unpack_from('<Q',elf,0x20)
    phentsize,phnum=struct.unpack_from('<HH',elf,0x36)
    segments=[]
    for i in range(phnum):
        kind,_,_,vaddr,_,_,memsz,_=struct.unpack_from('<IIQQQQQQ',elf,phoff+i*phentsize)
        if kind==1: segments.append((vaddr,vaddr+memsz))
    return segments


def encode(line):
    kind,value=line.get('Type'),line.get('Value')
    if kind=='bytes': return bytes.fromhex(value.replace(' ',''))
    if kind in ('bytes16','bytes32','bytes64'):
        return int(value,0).to_bytes(int(kind[5:])//8,'little')
    if kind=='float32': return struct.pack('<f',float(value))
    if kind=='float64': return struct.pack('<d',float(value))
    if kind=='utf8': return value.encode()+b'\0'
    if kind=='utf16': return value.encode('utf-16-le')+b'\0\0'
    raise ValueError(f'unsupported patch type {kind!r}')


def compile_patches(xml, names, app_version, segments):
    found={}
    for meta in ET.parse(xml).getroot().iter('Metadata'):
        if meta.get('Name') in names and meta.get('AppVer')==app_version and meta.get('AppElf','eboot.bin')=='eboot.bin':
            found[meta.get('Name')]=meta
    missing=[n for n in names if n not in found]
    if missing: raise ValueError(f'patches not found for app version {app_version}: {missing}')
    writes=[]
    for name in names:
        for line in found[name].iter('Line'):
            offset=int(line.get('Address'),0)-EBOOT_BASE
            data=encode(line)
            if not any(start<=offset and offset+len(data)<=end for start,end in segments):
                raise ValueError(f'{name}: address {line.get("Address")} is outside the eboot')
            writes.append((offset,data))
    return writes


# Third-party patch files (shadPS4/GoldHEN XML) in the data directory's patches/ folder.
# The retail releases (scripts/game_check.py SUPPORTED_TITLES): one 1.09 executable.
BLOODBORNE_IDS={'CUSA00900','CUSA00207','CUSA00208','CUSA00299','CUSA01363',
                'CUSA03179','CUSA03173','CUSA03014','CUSA03023'}


def external_patches(directory, app_version='01.09', exclude=Path(__file__).resolve().parent.parent/'patches/Bloodborne.xml'):
    """[(key, file, metadata)] of eboot patches for this version in directory/*.xml.
    key is "<file name>/<patch name>" (the launcher's selection, patches.json)."""
    found=[]
    directory=Path(directory)
    for path in sorted(directory.glob('*.xml')) if directory.is_dir() else []:
        if path.resolve()==Path(exclude).resolve(): continue  # the built-in file (patches/ in a checkout)
        try:
            root=ET.parse(path).getroot()
        except ET.ParseError as error:
            print(f'Patches: {path.name}: {error}',file=sys.stderr)
            continue
        ids={e.text.strip() for e in root.iter('ID') if e.text}
        if ids and not ids&BLOODBORNE_IDS: continue
        for meta in root.iter('Metadata'):
            if meta.get('AppVer')==app_version and meta.get('AppElf','eboot.bin')=='eboot.bin':
                found.append((f'{path.name}/{meta.get("Name")}',path,meta))
    return found


def external_selection(found, config):
    """Selected external patches: patches.json {"enabled": [...], "disabled": [...]} overrides
    each file's isEnabled."""
    settings={}
    if config and Path(config).is_file():
        settings=json.loads(Path(config).read_text(encoding='utf-8'))
    enabled,disabled=set(settings.get('enabled',[])),set(settings.get('disabled',[]))
    return [(key,path,meta) for key,path,meta in found
            if key in enabled or (key not in disabled and meta.get('isEnabled','false').lower()=='true')]


def compile_external(selected, segments):
    """Writes of the selected external patches; a patch with unsupported lines is skipped whole."""
    writes=[]
    for key,_,meta in selected:
        try:
            ours=[]
            for line in meta.iter('Line'):
                offset=int(line.get('Address') or '',0)-EBOOT_BASE
                data=encode(line)
                if not any(start<=offset and offset+len(data)<=end for start,end in segments):
                    raise ValueError(f'address {line.get("Address")} is outside the eboot')
                ours.append((offset,data))
        except ValueError as error:
            print(f'Patches: skipped {key}: {error}',file=sys.stderr)
            continue
        writes+=ours
        print(f'Patches: external {key} ({len(ours)} writes)')
    return writes


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--patches-dir',type=Path,help='third-party patch XML files (shadPS4 format)')
    p.add_argument('--patches-config',type=Path,help='patches.json: enabled/disabled external patches')
    p.add_argument('--xml',type=Path,default=Path(__file__).resolve().parent.parent/'patches/Bloodborne.xml')
    p.add_argument('--fps',choices=sorted(FPS_PRESETS),default='uncap')
    p.add_argument('--extra',default='',help='additional patch names, separated by ";"')
    p.add_argument('--app-version',default='01.09')
    p.add_argument('--out',type=Path,default=Path(__file__).resolve().parent.parent/'out')
    p.add_argument('--settings',type=Path,default=Path(__file__).resolve().parent.parent/'bbport.ini')
    p.add_argument('--game-dir',type=Path,default=Path(os.environ.get('BB_GAME_DIR','../CUSA03173')))
    p.add_argument('--render-res',default='',help='render resolution WxH (overrides the preset)')
    p.add_argument('--print-preset-size',action='store_true',help='print the selected preset size, if reduced')
    p.add_argument('--output-res',default='',help='output resolution WxH (the upscaler\'s; the UI stays 1920x1080)')
    p.add_argument('--print-scaled',action='store_true',
                   help='print "RENDER OUTPUT" (WxH) when bbport.ini selects an output other than 1080p')
    a=p.parse_args()
    if a.print_scaled:
        sizes=scaled_sizes(read_settings(a.settings))
        if sizes: print(f'{sizes[0][0]}x{sizes[0][1]} {sizes[1][0]}x{sizes[1][1]}')
        return
    if a.print_preset_size:
        settings=read_settings(a.settings)
        if 'BB_UPSCALER' in os.environ:
            settings['upscaler']='off' if os.environ['BB_UPSCALER'] in ('none','off') else os.environ['BB_UPSCALER']
        if 'BB_UPSCALE_PRESET' in os.environ:
            settings['preset']=os.environ['BB_UPSCALE_PRESET']
        size=render_size(settings)
        if size: print(f'{size[0]}x{size[1]}')
        return
    names=fps_patches(a.fps)+[n.strip() for n in a.extra.split(';') if n.strip()]  # bbcoop: BB_FPS_PATCH
    names+=[n for n in effect_patches(read_settings(a.settings)) if n not in names]
    if intel_tonemap_fix() and INTEL_TONEMAP not in names:
        names.append(INTEL_TONEMAP)
    if skip_network_choice() and SKIP_NETWORK_CHOICE not in names:
        names.append(SKIP_NETWORK_CHOICE)
    names=validate_patch_requirements(names,a.game_dir)
    segments=eboot_segments((a.out/'eboot.elf').read_bytes())
    writes=compile_patches(a.xml,names,a.app_version,segments)
    skip,variants,explicit=fps_choices()  # bbcoop: BB_FPS_SKIP / BB_FPS_TRY (default: DEFAULT_FPS_TRY)
    if (skip or variants) and ('Uncap FPS++' in names or explicit):
        if 'Uncap FPS++' not in names:
            raise ValueError('BB_FPS_SKIP / BB_FPS_TRY need the current Uncap FPS++ (BB_FPS=uncap, no BB_FPS_PATCH=old)')
        before=len(writes)
        uncap={o for o,_ in compile_patches(a.xml,['Uncap FPS++'],a.app_version,segments)}
        writes=apply_fps_choices(writes,skip,compile_patches(a.xml,variants,a.app_version,segments),uncap)
        print(f'Patches: BB_FPS_SKIP={os.environ.get("BB_FPS_SKIP","")} BB_FPS_TRY={os.environ.get("BB_FPS_TRY","")}: '
              f'{before} -> {len(writes)} writes')
        names+=variants
    if os.environ.get('BB_PATCH_BISECT'):  # bbcoop debug: drop units (a hook with its cave, or a plain line) lo..hi-1
        lo,hi=map(int,os.environ['BB_PATCH_BISECT'].split(':'))
        cur=[(o,d) for o,d in compile_patches(a.xml,['Uncap FPS++'],a.app_version,segments)]
        caves=[(o,o+len(d)) for o,d in cur if o+EBOOT_BASE>=0x5400000]
        def cave_of(t): return next((s for s,e in caves if s<=t<e),None)
        units={}; order=[]
        for o,d in cur:
            if o+EBOOT_BASE>=0x5400000: continue
            key=o; extra=[]
            if len(d)>=5 and d[0] in (0xE9,0xE8):
                t=(o+5+struct.unpack('<i',d[1:5])[0]) & 0xffffffff
                c=cave_of(t)
                if c is not None: extra=[c]
            units[key]=[o]+extra; order.append(key)
        drop=set()
        for k in order[lo:hi]: drop.update(units[k])
        n0=len(writes)
        writes=[w for w in writes if w[0] not in drop]
        print(f'Patches: BB_PATCH_BISECT={lo}:{hi}: dropped {n0-len(writes)} writes, {len(order)} units')
    size=render_size(read_settings(a.settings),a.render_res) if a.render_res else None
    # The UI keeps the game's 1920x1080 coordinates even for a larger output: the port draws
    # it into the output-size image with a viewport scaled by output / 1920
    # (UiComposition::NativeViewport), so it is rasterized at the output resolution.
    ui=OUTPUT_SIZE
    if size:
        writes+=resolution_writes(a.xml,size,a.app_version,segments,ui)
        if size[0]*size[1]>OUTPUT_SIZE[0]*OUTPUT_SIZE[1]:
            heap='Increased Graphics Heap Sizes'
            writes+=compile_patches(a.xml,[heap],a.app_version,segments)
            names.append(heap)
        print(f'Patches: scene {size[0]}x{size[1]}; UI {ui[0]}x{ui[1]}')
    if a.patches_dir:
        # After the built-in ones: an external patch of the same bytes wins.
        writes+=compile_external(external_selection(external_patches(a.patches_dir,a.app_version,a.xml),
                                                    a.patches_config),segments)
    # BBPATCH2: the patch base, so the loader can rebase pointers the patches write into
    # relocated slots (60/90 FPS++ replace function pointers).
    blob=struct.pack('<8sQQ',b'BBPATCH2',EBOOT_BASE,len(writes))
    for offset,data in writes: blob+=struct.pack('<QQ',offset,len(data))+data
    (a.out/'patches.bin').write_bytes(blob)
    print(f'Patches: FPS preset {a.fps}; {len(writes)} writes from {names or "none"}')


if __name__=='__main__':
    main()
