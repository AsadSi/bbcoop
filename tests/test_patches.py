from paths import ROOT
import re
import struct
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

import patches

from patches import (EBOOT_BASE, OUTPUT_SIZE, RESOLUTION_TEMPLATE, SCENE_HEIGHT,
                     SCENE_WIDTH, UI_HEIGHT, UI_WIDTH, compile_patches,
                     render_size, resolution_writes, scaled_sizes, effect_patches,
                     validate_patch_requirements, external_patches, external_selection,
                     compile_external)

XML = ROOT / 'patches/Bloodborne.xml'
SEGMENTS = [(0, 0x6000000)]


def immediate(writes, address):
    # The XML splits a 9-byte MOV imm32 + NOP into 4+4+1 byte writes.
    data = bytearray(9)
    for offset, value in writes:
        if address <= offset < address + 9:
            data[offset-address:offset-address+len(value)] = value
    assert data[5:] == b'\x0f\x1f\x40\x00'
    return data[0], struct.unpack_from('<I', data, 1)[0]


class NativeUiTests(unittest.TestCase):
    def test_presets_keep_ui_native(self):
        for preset, expected in [(1, (1280, 720)), (2, (1130, 636)),
                                 (3, (960, 540)), (4, (640, 360))]:
            with self.subTest(preset=preset):
                size = render_size({'upscaler': 'fsr3', 'preset': str(preset)})
                self.assertEqual(size, expected)
                writes = resolution_writes(XML, size, '01.09', SEGMENTS)
                self.assertEqual(immediate(writes, SCENE_WIDTH), (0xB8, size[0]))
                self.assertEqual(immediate(writes, SCENE_HEIGHT), (0xB8, size[1]))
                self.assertEqual(immediate(writes, UI_WIDTH), (0xB8, OUTPUT_SIZE[0]))
                self.assertEqual(immediate(writes, UI_HEIGHT), (0xB9, OUTPUT_SIZE[1]))

    def test_explicit_scene_resolution_keeps_native_ui(self):
        writes = resolution_writes(XML, (800, 450), '01.09', SEGMENTS)
        self.assertEqual(immediate(writes, SCENE_WIDTH)[1], 800)
        self.assertEqual(immediate(writes, UI_WIDTH)[1], 1920)
        self.assertEqual(immediate(writes, UI_HEIGHT)[1], 1080)

    def test_coordinate_and_aspect_fixes_are_preserved(self):
        original = compile_patches(XML, [RESOLUTION_TEMPLATE], '01.09', SEGMENTS)
        modified = resolution_writes(XML, (960, 540), '01.09', SEGMENTS)
        self.assertEqual(len(original), len(modified))
        changed = {SCENE_WIDTH, SCENE_HEIGHT, UI_WIDTH, UI_HEIGHT}
        self.assertEqual([w for w in original if w[0] not in changed],
                         [w for w in modified if w[0] not in changed])

    def test_unexpected_or_missing_ui_instruction_is_rejected(self):
        for remove in (False, True):
            tree = ET.parse(XML)
            for meta in tree.getroot().iter('Metadata'):
                if meta.get('Name') == RESOLUTION_TEMPLATE and meta.get('AppVer') == '01.09':
                    patch_list = meta.find('PatchList')
                    for line in list(patch_list):
                        if int(line.get('Address'), 0) == UI_WIDTH + EBOOT_BASE:
                            if remove:
                                patch_list.remove(line)
                            else:
                                line.set('Value', '0x000500B9')
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / 'patch.xml'
                tree.write(path)
                with self.assertRaises(ValueError):
                    resolution_writes(path, (960, 540), '01.09', SEGMENTS)

    def test_native_and_disabled_upscaler_need_no_resolution_patch(self):
        self.assertIsNone(render_size({'preset': '0'}))
        self.assertIsNone(render_size({'upscaler': 'off', 'preset': '3'}))

    def test_output_other_than_1080p_scales_the_scene(self):
        self.assertIsNone(scaled_sizes({'output_res': '1920x1080', 'preset': '2'}))
        # Steam Deck: below 1080p the scene is still the preset's fraction of the output.
        for preset, expected in [(0, (1280, 720)), (2, (752, 424)), (4, (426, 240))]:
            with self.subTest(preset=preset):
                self.assertEqual(scaled_sizes({'output_res': '1280x720', 'preset': str(preset)}),
                                 (expected, (1280, 720)))
        self.assertEqual(scaled_sizes({'output_res': '3840x2160', 'preset': '3'}),
                         ((1916, 1078), (3840, 2160)))
        # TAA is native-only and uses the live host targets.
        self.assertIsNone(scaled_sizes({'output_res': '1280x720', 'upscaler': 'taa', 'preset': '3'}))


class DebugPatchTests(unittest.TestCase):
    def test_ssr_uses_complete_startup_patch_with_live_effects(self):
        for env in ({}, {'BB_HOT_EFFECTS': '0'}):
            names = effect_patches({'effect_ssr': '1'}, env)
            writes = dict(compile_patches(XML, names, '01.09', SEGMENTS))
            # Replace both dimension-halving instructions before allocation.
            self.assertEqual(writes[0x22c428d], bytes.fromhex('0f1f4000'))
            self.assertEqual(len(writes), 10)
            self.assertEqual(effect_patches({'effect_ssr': '0'}, env), [])
        self.assertEqual(effect_patches({'effect_bloom': '0'}, {}), [])

    def test_camera_patch_is_optional_and_compatible_with_fps_and_debug_menu(self):
        self.assertEqual(effect_patches({'debug_camera': '0', 'debug_menu': '0'}), [])
        camera = effect_patches({'debug_camera': '1'})
        writes = compile_patches(XML, camera, '01.09', SEGMENTS)
        self.assertGreater(len(writes), 0)
        camera_bytes = {offset+i: byte for offset, data in writes for i, byte in enumerate(data)}
        for patch in ('Uncap FPS++', 'Uncap FPS++ (2026-08-28)', '60 FPS++', '90 FPS++', '120 FPS++',
                      'Restore Debug Menu (READ NOTES)'):
            for offset, data in compile_patches(XML, [patch], '01.09', SEGMENTS):
                for i, byte in enumerate(data):
                    if offset+i in camera_bytes:
                        self.assertEqual(camera_bytes[offset+i], byte, patch)

    def test_debug_menu_needs_both_fonts_where_the_game_reads_them(self):
        names = effect_patches({'debug_menu': '1'})
        menu = 'Restore Debug Menu (READ NOTES)'
        with tempfile.TemporaryDirectory() as directory:
            game = Path(directory)
            self.assertEqual(validate_patch_requirements(['Restore Debug Camera'], game),
                             ['Restore Debug Camera'])
            # Without the fonts the patch is left out (the game would crash opening the menu).
            self.assertNotIn(menu, validate_patch_requirements(names, game))
            # The old instructions' folder: still left out, the game reads adhoc/font.
            old = game / 'dvdroot_ps4/font'
            old.mkdir(parents=True)
            (old / 'DbgFont14h.ccm').write_bytes(b'test')
            (old / 'DbgFont14h.tpf').write_bytes(b'test')
            self.assertNotIn(menu, validate_patch_requirements(names, game))
            # A mod's own case (Adhoc/Font/dbgfont14h.*) is found; an empty file is not enough.
            font = game / 'dvdroot_ps4/Adhoc/Font'
            font.mkdir(parents=True)
            (font / 'dbgfont14h.ccm').write_bytes(b'test')
            (font / 'dbgfont14h.tpf').touch()
            self.assertNotIn(menu, validate_patch_requirements(names, game))
            (font / 'dbgfont14h.tpf').write_bytes(b'test')
            self.assertIn(menu, validate_patch_requirements(names, game))

    def test_conflicting_enemy_control_patch_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'conflicts with Enemy Control'):
            validate_patch_requirements(['Enemy Control', 'Restore Debug Camera'], Path('.'))


def covered(names):
    return {offset+i: byte for offset, data in compile_patches(XML, names, '01.09', SEGMENTS)
            for i, byte in enumerate(data)}


def runtime_sites():
    """(file, name, image offset, length) of the byte-checked sites in src/runtime_*.c."""
    sites = []
    for path in sorted((ROOT / 'src').glob('runtime_*.c')):
        source = path.read_text(encoding='utf-8')
        defines = dict(re.findall(r'#define\s+(\w+)\s+(0x[0-9A-Fa-f]+)', source))
        for name, offset, length in re.findall(r'\{\s*"([^"]+)"\s*,\s*(\w+)\s*,\s*(\d+)\s*,', source):
            value = defines.get(offset, offset)
            if value.startswith('0x'):
                sites.append((path.name, name, int(value, 16), int(length)))
    # Hook entries not in a {"name", offset, length} table.
    sites += [('runtime_camera.c', 'camera update entry', 0x143ac60, 10),
              ('runtime_ik.c', 'ChrCtrl update entry (hook)', 0x1514da0, 10)]
    return sites


class FpsPatchTests(unittest.TestCase):
    """bbcoop: with the frame rate hooks off (BB_FRAME_HOOKS=0), the current Uncap FPS++ (ps4_cheats
    2026-10-10), BB_FPS_PATCH=old the previous."""
    NEW = ['Uncap FPS++']
    OLD = ['Uncap FPS++ (2026-08-28)', 'Sprint Fix (High FPS)']
    PATCHES = {'BB_FRAME_HOOKS': '0'}

    def test_uncap_preset_selects_the_current_patch_by_default(self):
        self.assertEqual(patches.fps_patches('uncap', self.PATCHES), self.NEW)
        self.assertEqual(patches.fps_patches('uncap', {**self.PATCHES, 'BB_FPS_PATCH': 'new'}), self.NEW)
        # The sprint fix is part of it now.
        sprint = covered(['Sprint Fix (High FPS)'])
        uncap = covered(self.NEW)
        self.assertTrue(all(uncap.get(a) == b for a, b in sprint.items()))

    def test_old_switch_selects_the_previous_patch(self):
        self.assertEqual(patches.fps_patches('uncap', {**self.PATCHES, 'BB_FPS_PATCH': 'old'}), self.OLD)
        # Only the uncapped preset has an old version.
        self.assertEqual(patches.fps_patches('90', {**self.PATCHES, 'BB_FPS_PATCH': 'old'}), patches.FPS_PRESETS['90'])
        self.assertEqual(patches.fps_patches('120', self.PATCHES), ['120 FPS++'])
        # The hooks (default) take no patches, old or new.
        self.assertEqual(patches.fps_patches('uncap', {'BB_FPS_PATCH': 'old'}), [])
        for names in (self.NEW, self.OLD, ['120 FPS++']):
            self.assertGreater(len(compile_patches(XML, names, '01.09', SEGMENTS)), 100, names)

    def test_no_overlap_with_the_other_uncap_patches_or_runtime_sites(self):
        others = [n for pair in patches.EFFECTS.values() for n in pair if n]
        others += list(patches.MODEL_LOD.values())
        others += [patches.INTEL_TONEMAP, patches.SKIP_NETWORK_CHOICE, RESOLUTION_TEMPLATE,
                   'Increased Graphics Heap Sizes']
        sites = runtime_sites()
        self.assertGreater(len(sites), 40)
        for names in (self.NEW, self.OLD, ['120 FPS++']):
            fps = covered(names)
            for other in others:
                with self.subTest(fps=names[0], other=other):
                    self.assertFalse(set(fps) & set(covered([other])))
            for path, name, offset, length in sites:
                with self.subTest(fps=names[0], site=name):
                    self.assertFalse(set(range(offset, offset + length)) & set(fps), path)

    def test_skip_groups_are_lines_of_the_current_patch(self):
        # Every group address starts a line of Uncap FPS++; groups don't share lines.
        meta = next(m for m in ET.parse(XML).getroot().iter('Metadata') if m.get('Name') == 'Uncap FPS++')
        starts = {int(line.get('Address'), 0) for line in meta.iter('Line')}
        seen = set()
        for group, addresses in patches.FPS_SKIP_GROUPS.items():
            with self.subTest(group=group):
                self.assertTrue(set(addresses) <= starts, [hex(a) for a in set(addresses) - starts])
                self.assertFalse(seen & set(addresses))
                seen |= set(addresses)

    def test_skip_and_try_switches(self):
        # Default: the wind and cloth fixes; "none" or an empty value: Uncap FPS++ as published.
        self.assertEqual(patches.fps_choices({}), (set(), ['bbcoop: Fixed Wind Offset Step 1/60',
                                                           'bbcoop: Fixed Cloth Step 1/120',
                                                           'bbcoop: Frame Time Floor 1/240'], False))
        self.assertEqual(patches.fps_choices({'BB_FPS_TRY': 'none'}), (set(), [], True))
        self.assertEqual(patches.fps_choices({'BB_FPS_TRY': ''}), (set(), [], True))
        skip, variants, _ = patches.fps_choices({'BB_FPS_SKIP': 'grass, cloth', 'BB_FPS_TRY': 'step1972'})
        self.assertEqual(skip, set(patches.FPS_SKIP_GROUPS['grass']) | set(patches.FPS_SKIP_GROUPS['cloth']))
        self.assertEqual(variants, ['bbcoop: Frame Time Step at 0x1972A7D'])
        with self.assertRaisesRegex(ValueError, 'BB_FPS_SKIP'):
            patches.fps_choices({'BB_FPS_SKIP': 'flowers'})
        with self.assertRaisesRegex(ValueError, 'BB_FPS_TRY'):
            patches.fps_choices({'BB_FPS_TRY': 'wind'})
        writes = compile_patches(XML, self.NEW, '01.09', SEGMENTS)
        offsets = [o for o, _ in writes]
        for group, addresses in patches.FPS_SKIP_GROUPS.items():
            kept = patches.apply_fps_choices(writes, set(addresses), [])
            self.assertEqual(len(kept), len(writes) - len(addresses), group)
            # No other line still writes part of a skipped site: it keeps the game's bytes.
            dropped = {o + i for o, data in writes if o + EBOOT_BASE in addresses for i in range(len(data))}
            self.assertFalse(dropped & {o + i for o, data in kept for i in range(len(data))}, group)
        for key, name in patches.FPS_TRY_VARIANTS.items():
            variant = compile_patches(XML, [name], '01.09', SEGMENTS)
            result = patches.apply_fps_choices(writes, set(), variant)
            with self.subTest(variant=key):
                self.assertEqual(result[-len(variant):], variant)
                # The variant's bytes are the only writes to its sites (Uncap FPS++ itself has overlapping lines).
                sites = {o + i for o, data in variant for i in range(len(data))}
                others = {o + i for o, data in result[:-len(variant)] for i in range(len(data))}
                self.assertFalse(sites & others)
        # step1972 replaces Uncap FPS++'s 0x01972A7D line (the same 7 bytes).
        step = compile_patches(XML, [patches.FPS_TRY_VARIANTS['step1972']], '01.09', SEGMENTS)
        result = patches.apply_fps_choices(writes, set(), step, set(offsets))
        self.assertIn(0x01972A7D - EBOOT_BASE, offsets)
        self.assertEqual([d for o, d in result if o == 0x01972A7D - EBOOT_BASE], [bytes.fromhex('E97EE0B1039090')])
        # Variants that overlap each other, or cut a line that is not Uncap FPS++'s, are refused.
        wind = [compile_patches(XML, [patches.FPS_TRY_VARIANTS[k]], '01.09', SEGMENTS) for k in ('wind30', 'wind60')]
        with self.assertRaisesRegex(ValueError, 'overlap'):
            patches.apply_fps_choices(writes, set(), wind[0] + wind[1])
        enemy = compile_patches(XML, ['Enemy Control'], '01.09', SEGMENTS)
        with self.assertRaisesRegex(ValueError, 'cut'):
            patches.apply_fps_choices(writes + enemy, set(), step, set(offsets))

    def test_try_variant_branch_targets(self):
        # The hand-assembled rel32 targets: jmp/call/jcc rel32 and RIP-relative loads.
        def target(address, data, at, size):
            return address + at + size + struct.unpack_from('<i', data, at)[0]
        name = patches.FPS_TRY_VARIANTS['step1972']
        lines = {o + EBOOT_BASE: d for o, d in compile_patches(XML, [name], '01.09', SEGMENTS)}
        self.assertEqual(target(0x01972A7D, lines[0x01972A7D], 1, 4), 0x05490B00)
        cave = lines[0x05490B00]
        self.assertEqual(target(0x05490B00, cave, 2, 4), 0x0575A908)
        self.assertEqual(target(0x05490B00, cave, 10, 4), 0x01972A84)
        for key, constant in (('wind30', 0x04D26364), ('wind60', 0x04D29E70)):
            lines = {o + EBOOT_BASE: d for o, d in
                     compile_patches(XML, [patches.FPS_TRY_VARIANTS[key]], '01.09', SEGMENTS)}
            self.assertEqual(target(0x027149EA, lines[0x027149EA], 4, 4), constant, key)
            rewrite = lines[0x02714B26]
            self.assertEqual(target(0x02714B26, rewrite, 14, 5), 0x05940DF9, key)  # cmp byte [rip+x], 0
            self.assertEqual(0x02714B26 + 21 + rewrite[20], 0x02714B43, key)       # jne rel8
        # Nothing else in the file writes the variants' sites or the cave.
        names = [m.get('Name') for m in ET.parse(XML).getroot().iter('Metadata')]
        ours = set(patches.FPS_TRY_VARIANTS.values())
        sites = set().union(*(covered([n]) for n in ours))
        # Other frame rate patches never go with Uncap FPS++; Enemy Control is refused by apply_fps_choices.
        for other in names:
            if other in ours or 'FPS' in other or other == 'Enemy Control':
                continue
            with self.subTest(other=other):
                try:
                    theirs = covered([other])
                except ValueError:
                    continue
                self.assertFalse(sites & set(theirs))
        for path, name, offset, length in runtime_sites():
            self.assertFalse(set(range(offset, offset + length)) & sites, name)

    def test_try_variants_fit_the_game_and_the_patch(self):
        # Cave after Uncap FPS++'s own, outside every other patch.
        uncap = covered(self.NEW)
        for key, name in patches.FPS_TRY_VARIANTS.items():
            ours = covered([name])
            with self.subTest(variant=key):
                outside = {a for a in ours if a not in uncap}
                if key == 'step1972':
                    cave = set(range(0x05490B00 - EBOOT_BASE, 0x05490B0E - EBOOT_BASE))
                    self.assertEqual(outside, cave)
                    self.assertGreater(min(cave), max(a for a in uncap if a < 0x054A0000 - EBOOT_BASE))
                elif key == 'clothstep':
                    # Its caves lie after the step1972 cave; inside Uncap FPS++ it replaces whole cloth lines.
                    caves = {a for a in outside if a >= 0x05490000 - EBOOT_BASE}
                    self.assertEqual(min(caves), 0x05490B10 - EBOOT_BASE)
                    writes = compile_patches(XML, self.NEW, '01.09', SEGMENTS)
                    variant = compile_patches(XML, [name], '01.09', SEGMENTS)
                    replaced = {o + EBOOT_BASE for o, d in writes if any(o + i in ours for i in range(len(d)))}
                    self.assertEqual(replaced, set(patches.FPS_SKIP_GROUPS['cloth']))
                    patches.apply_fps_choices(writes, set(), variant, {o for o, _ in writes})
                elif key in ('dtfloor', 'ctx30'):
                    # Same hook as Uncap FPS++'s frame time cave (46 bytes at 0x02418E3D, replaced), its own
                    # cave after the cloth ones.
                    caves = {a for a in outside if a >= 0x05490000 - EBOOT_BASE}
                    self.assertGreaterEqual(min(caves), 0x05490C00 - EBOOT_BASE)
                    hook = set(range(0x02418E3D - EBOOT_BASE, 0x02418E3D - EBOOT_BASE + 46))
                    self.assertEqual(outside - caves, set())
                    self.assertEqual(set(ours) & set(uncap), hook)
                else:
                    # The wind step sites are vanilla in Uncap FPS++.
                    self.assertEqual(outside, set(ours))
        eboot = ROOT / 'out/eboot.elf'
        if not eboot.is_file():
            self.skipTest('needs out/eboot.elf')
        elf = eboot.read_bytes()
        text = lambda address, n: elf[address - EBOOT_BASE + 0x4000:address - EBOOT_BASE + 0x4000 + n]
        # The game's bytes at the sites the variants rewrite, and a free cave.
        self.assertEqual(text(0x01972A7D, 7).hex(), 'c745d08988083d')
        self.assertEqual(text(0x027149EA, 8).hex(), 'c5796fb590feffff')
        self.assertEqual(text(0x02714B26, 29).hex(),
                         '488d05ccc22203803800c4412857d2448b8548feffff7505c441796fd6')
        self.assertEqual(text(0x05490B00, 14), bytes(14))
        # clothstep: the cloth manager's dt store, the pre-step's and adapter's substep reads, free caves,
        # and its two floats in the zero RTTI slot of vtable 0x578CF60 (data segment).
        self.assertEqual(text(0x013D545B, 8).hex(), 'c5fa118344010000')
        self.assertEqual(text(0x00B6FBF0, 11).hex(), '8b401885c07504418b4720')
        self.assertEqual(text(0x00B72225, 16).hex(), '8b40188945cc85c075068b43208945cc')
        self.assertEqual(text(0x05490B10, 0x190), bytes(0x190))
        data = 0x0578CF58 - EBOOT_BASE - 0x50dc000 + 0x50e0000
        self.assertEqual(elf[data:data + 8], bytes(8))

    def test_dead_broken_call_is_left_out(self):
        # Upstream's 0x02715D71 call has its rel32 overwritten by the following 0x02715D75 line.
        writes = compile_patches(XML, self.NEW, '01.09', SEGMENTS)
        self.assertNotIn(0x02715D71 - EBOOT_BASE, [offset for offset, _ in writes])
        self.assertEqual(covered(self.NEW)[0x02715D75 - EBOOT_BASE], 0x8F)


EXTERNAL = """<?xml version="1.0"?>
<Patch>
  <TitleID><ID>CUSA03173</ID><ID>CUSA00207</ID></TitleID>
  <Metadata Title="Bloodborne" Name="On" Author="x" PatchVer="1.0" AppVer="01.09" AppElf="eboot.bin" isEnabled="true">
    <PatchList><Line Type="bytes" Address="0x00401000" Value="9090"/></PatchList>
  </Metadata>
  <Metadata Title="Bloodborne" Name="Off" Author="x" PatchVer="1.0" AppVer="01.09" AppElf="eboot.bin">
    <PatchList><Line Type="bytes32" Address="0x00402000" Value="0x12345678"/></PatchList>
  </Metadata>
  <Metadata Title="Bloodborne" Name="Mask" Author="x" PatchVer="1.0" AppVer="01.09" AppElf="eboot.bin" isEnabled="true">
    <PatchList><Line Type="mask" Value="90 ?? 90" Offset="0"/></PatchList>
  </Metadata>
  <Metadata Title="Bloodborne" Name="Old" Author="x" PatchVer="1.0" AppVer="01.00" AppElf="eboot.bin" isEnabled="true">
    <PatchList><Line Type="bytes" Address="0x00403000" Value="90"/></PatchList>
  </Metadata>
</Patch>"""


class ExternalPatchTests(unittest.TestCase):
    def test_selection_and_unsupported_lines(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            (folder / 'extra.xml').write_text(EXTERNAL)
            (folder / 'other.xml').write_text(EXTERNAL.replace('CUSA03173', 'CUSA99999')
                                              .replace('CUSA00207', 'CUSA99998'))
            (folder / 'broken.xml').write_text('<Patch>')
            found = external_patches(folder)
            self.assertEqual([key for key, _, _ in found],
                             ['extra.xml/On', 'extra.xml/Off', 'extra.xml/Mask'])
            # The file's isEnabled; the mask patch is skipped as a whole.
            writes = compile_external(external_selection(found, None), SEGMENTS)
            self.assertEqual(writes, [(0x1000, bytes.fromhex('9090'))])
            config = folder / 'patches.json'
            config.write_text('{"enabled": ["extra.xml/Off"], "disabled": ["extra.xml/On"]}')
            writes = compile_external(external_selection(found, config), SEGMENTS)
            self.assertEqual(writes, [(0x2000, (0x12345678).to_bytes(4, 'little'))])

    def test_built_in_file_is_not_external(self):
        self.assertEqual(external_patches(XML.parent), [])


class FrameRateTests(unittest.TestCase):
    def test_frame_rate_needs_no_patches_but_with_hooks_off(self):
        # Above 30 FPS hooks in the game's code run the frame rate (gpu/shim/bbport_frame_rate.cpp).
        for fps in ('uncap', '60', '90', '120', '30'):  # bbcoop: 120
            self.assertEqual(patches.fps_patches(fps, {}), [], fps)
        # bbcoop: its Uncap FPS++ has the sprint fix built in (FpsPatchTests).
        self.assertEqual(patches.fps_patches('uncap', {'BB_FRAME_HOOKS': '0'}), ['Uncap FPS++'])
        self.assertEqual(patches.fps_patches('60', {'BB_FRAME_HOOKS': '0'}), ['60 FPS++'])


class IntelTonemapTests(unittest.TestCase):
    def cpuinfo(self, vendor):
        path = Path(tempfile.mkdtemp()) / 'cpuinfo'
        path.write_text(f'processor\t: 0\nvendor_id\t: {vendor}\nmodel name\t: x\n')
        return str(path)

    def test_on_for_intel_off_for_amd(self):
        self.assertTrue(patches.intel_tonemap_fix({}, self.cpuinfo('GenuineIntel')))
        self.assertFalse(patches.intel_tonemap_fix({}, self.cpuinfo('AuthenticAMD')))

    def test_environment_forces_it(self):
        amd, intel = self.cpuinfo('AuthenticAMD'), self.cpuinfo('GenuineIntel')
        self.assertTrue(patches.intel_tonemap_fix({'BB_INTEL_TONEMAP_FIX': '1'}, amd))
        self.assertFalse(patches.intel_tonemap_fix({'BB_INTEL_TONEMAP_FIX': '0'}, intel))

    def test_patch_exists_for_109(self):
        xml = ET.parse(ROOT / 'patches/Bloodborne.xml')
        names = [m.get('Name') for m in xml.iter('Metadata')]
        self.assertIn(patches.INTEL_TONEMAP, names)


class GxCommandHeapTests(unittest.TestCase):
    def test_compiled_patch_changes_only_the_two_heap_size_operands(self):
        writes = dict(compile_patches(XML, [patches.GX_COMMAND_HEAP], '01.09', SEGMENTS))
        expected = {0x2195c0a: struct.pack('<I', 1024 * 1024),
                    0x2195c47: struct.pack('<I', 1024 * 1024)}
        self.assertEqual(writes, expected)


if __name__ == '__main__':
    unittest.main()
