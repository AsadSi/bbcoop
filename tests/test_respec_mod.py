"""bbcoop: tools/respec_mod.py on the game's own files. Needs the game (BB_GAME_DIR, or the US or EU
folder next to the repository) and soulstruct in BB_RE_PYTHON (default ~/.local/share/bbcoop-re);
skipped without them. The script itself checks that only the Doll's and the workbench's talk scripts and
the EventText and MenuDialogs_SP entries change, and in the gameparam only the firearms' WeaponGenParam rows; this test checks the result
once more from the outside."""
from paths import ROOT
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

PYTHON = Path(os.environ.get('BB_RE_PYTHON', Path.home() / '.local/share/bbcoop-re/bin/python'))
CANDIDATES = [os.environ.get('BB_GAME_DIR', ''), ROOT.parent / 'CUSA03173', ROOT.parent / 'CUSA00900']
GAME = next((Path(c) for c in CANDIDATES if c and (Path(c) / 'dvdroot_ps4/script/talk').is_dir()), None)
TALK_AND_MSG = ['dvdroot_ps4/msg/engus/menu.msgbnd.dcx', 'dvdroot_ps4/script/talk/m21_00_00_00.talkesdbnd.dcx']
GAMEPARAM = 'dvdroot_ps4/param/gameparam/gameparam.parambnd.dcx'

# WeaponGenParam rows of a gameparam as {row id: [row bytes (hex), open gem slot types]}, read with soulstruct.
ROWS = r'''
import json, sys
from pathlib import PureWindowsPath
from soulstruct.containers import Binder
from soulstruct.base.params.param import TypedParam
from soulstruct.bloodborne.params.paramdef import WEAPON_GEN_PARAM_ST
bnd = Binder.from_path(sys.argv[1])
entry = [e for e in bnd.entries if PureWindowsPath(e.path).name == 'WeaponGenParam.param'][0]
rows = TypedParam(WEAPON_GEN_PARAM_ST).from_bytes(entry.get_uncompressed_data()).rows
print(json.dumps({i: [bytes(r.to_bytes()).hex(), [t for t in (r[f'gemSlotType_{s}'] for s in range(5))
                                                  if not t & 0x80000000]] for i, r in rows.items()}))
'''

PISTOL = 1400
FIREARMS = ['Hunter Blunderbuss', "Ludwig's Rifle", '(no name)', 'Hunter Pistol', 'Evelyn', 'Repeating Pistol',
            'Cannon', 'Rosmarinus', 'Flamesprayer', "Hunter's Torch", 'Gatling Gun', 'Fist of Gratia', 'Church Cannon',
            'Piercing Rifle']
SLOTLESS = {1500: 'Cannon', 1800: 'Rosmarinus', 1810: 'Flamesprayer', 2000: "Hunter's Torch", 3400: 'Fist of Gratia',
            3500: 'Church Cannon'}
SHIELDS_AND_TORCH = [19000000, 19100000, 20100000]  # Wooden Shield, Loch Shield, Torch: +0 only
MELEE = [7000000 + level * 100 for level in range(11)] + [22010000 + level * 100 for level in range(11)]


def build(out, **env):
    environment = {k: v for k, v in os.environ.items() if k != 'BB_GUN_GEMS'} | env
    return subprocess.run([str(PYTHON), '-I', str(ROOT / 'tools/respec_mod.py'), str(GAME), out],
                          capture_output=True, text=True, env=environment)


def rows(path):
    run = subprocess.run([str(PYTHON), '-I', '-c', ROWS, str(path)], capture_output=True, text=True)
    assert run.returncode == 0, run.stderr
    # The last line: soulstruct logs to stdout too.
    return {int(k): v for k, v in json.loads(run.stdout.splitlines()[-1]).items()}


def files(out):
    return sorted(str(p.relative_to(out)) for p in Path(out).rglob('*') if p.is_file())


@unittest.skipUnless(PYTHON.exists() and GAME, 'needs the game files and soulstruct (BB_GAME_DIR, BB_RE_PYTHON)')
class RespecModTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.out = cls.tmp.name
        cls.result = build(cls.out)
        if cls.result.returncode == 0:
            cls.old = rows(GAME / 'dvdroot_ps4/param/gameparam/gameparam.parambnd.dcx')
            cls.new = rows(Path(cls.out) / GAMEPARAM)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        self.assertEqual(self.result.returncode, 0, self.result.stdout + self.result.stderr)

    def test_builds_the_three_files(self):
        # Reset Attributes at the Doll, Refund Weapon Upgrades at the workbench (just before Leave).
        self.assertIn('talk OK: only t210304.esd (x7 states [2, 5, 31, 32], labels [15000100, 15000007, 15000008, '
                      '15009100, 15000005]) and t210692.esd (x2 states [1, 2, 3, 6, 27, 28, 29], labels [15000110, '
                      '15000120, 15000111, 10011050, 15009110, 15000005]) changed', self.result.stdout)
        self.assertIn("'Reset Attributes'", self.result.stdout)
        self.assertIn("'Refund Weapon Upgrades'", self.result.stdout)
        # The refund confirm per level lists what comes back (the game's names and EquipMtrlSetParam counts).
        self.assertIn('221091: "Refund this weapon\'s upgrades? You get back:\\nBlood Stone Shard × 3"', self.result.stdout)
        self.assertIn('221100: "Refund this weapon\'s upgrades? You get back:\\nBlood Stone Shard × 16\\n'
                      'Twin Blood Stone Shards × 16\\nBlood Stone Chunk × 16\\nBlood Rock × 1"', self.result.stdout)
        self.assertEqual(files(self.out), sorted(TALK_AND_MSG + [GAMEPARAM]))

    def test_firearms_are_upgradable_left_hand_weapons(self):
        line = next(l for l in self.result.stdout.splitlines() if l.startswith('Gun gems: firearm families: '))
        self.assertEqual([f.split(' ', 1)[1] for f in line.split(': ', 2)[2].split(', ')], FIREARMS)
        self.assertIn('Gun gems: 60 rows in 6 firearm families', self.result.stdout)

    def test_slotless_firearms_get_the_pistol_slot(self):
        for family in SLOTLESS:
            for level in range(1, 11):
                row, pistol = family * 10000 + level * 100, PISTOL * 10000 + level * 100
                self.assertEqual(self.old[row][1], [], row)
                self.assertEqual(self.new[row][1], self.new[pistol][1], row)
                self.assertEqual(len(self.new[row][1]), 1, row)
                # Same slot fields as the pistol (wepParamId is the first 4 bytes).
                self.assertEqual(self.new[row][0][8:], self.new[pistol][0][8:], row)
                self.assertEqual(self.new[row][0][:8], self.old[row][0][:8], row)
        self.assertEqual(self.new[15000100][1], [8])  # Cannon +1: the pistol's droplet slot
        self.assertEqual(self.new[20001000][1], [8])  # Hunter's Torch +10

    def test_only_those_rows_change(self):
        changed = {row for row in self.old if self.old[row] != self.new[row]}
        self.assertEqual(changed, {f * 10000 + level * 100 for f in SLOTLESS for level in range(1, 11)})
        self.assertEqual(set(self.old), set(self.new))
        for row in [PISTOL * 10000 + level * 100 for level in range(11)] + SHIELDS_AND_TORCH + MELEE:
            self.assertEqual(self.new[row], self.old[row], row)
        for family in SLOTLESS:
            self.assertEqual(self.new[family * 10000], self.old[family * 10000])  # +0
            self.assertEqual(self.new[family * 10000][1], [])

    def test_gun_gems_off(self):
        with tempfile.TemporaryDirectory() as out:
            run = build(out, BB_GUN_GEMS='0')
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('Gun gems: off (BB_GUN_GEMS=0)', run.stdout)
            self.assertEqual(files(out), TALK_AND_MSG)
            # A gameparam left from an earlier build into the same folder is removed.
            (Path(out) / GAMEPARAM).parent.mkdir(parents=True)
            (Path(out) / GAMEPARAM).write_bytes(b'old')
            run = build(out, BB_GUN_GEMS='0')
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertEqual(files(out), TALK_AND_MSG)

    def test_refuses_a_wrong_folder(self):
        with tempfile.TemporaryDirectory() as out:
            run = subprocess.run([str(PYTHON), '-I', str(ROOT / 'tools/respec_mod.py'), out, out],
                                 capture_output=True, text=True)
            self.assertNotEqual(run.returncode, 0)


if __name__ == '__main__':
    unittest.main()
