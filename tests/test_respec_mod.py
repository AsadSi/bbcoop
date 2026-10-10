"""bbcoop: tools/respec_mod.py on the game's own files. Needs the game (BB_GAME_DIR, or the US or EU
folder next to the repository) and soulstruct in BB_RE_PYTHON (default ~/.local/share/bbcoop-re);
skipped without them. The script itself checks that only the Doll's talk script and the EventText
entry change; this test checks the result once more from the outside."""
from paths import ROOT
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

PYTHON = Path(os.environ.get('BB_RE_PYTHON', Path.home() / '.local/share/bbcoop-re/bin/python'))
CANDIDATES = [os.environ.get('BB_GAME_DIR', ''), ROOT.parent / 'CUSA03173', ROOT.parent / 'CUSA00900']
GAME = next((Path(c) for c in CANDIDATES if c and (Path(c) / 'dvdroot_ps4/script/talk').is_dir()), None)


@unittest.skipUnless(PYTHON.exists() and GAME, 'needs the game files and soulstruct (BB_GAME_DIR, BB_RE_PYTHON)')
class RespecModTests(unittest.TestCase):
    def test_builds_the_two_files(self):
        with tempfile.TemporaryDirectory() as out:
            run = subprocess.run([str(PYTHON), '-I', str(ROOT / 'tools/respec_mod.py'), str(GAME), out],
                                 capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('talk OK: only t210304.esd changed', run.stdout)
            self.assertIn("'Reset Attributes'", run.stdout)
            self.assertIn("'Refund Weapon Upgrades'", run.stdout)
            files = sorted(str(p.relative_to(out)) for p in Path(out).rglob('*') if p.is_file())
            self.assertEqual(files, ['dvdroot_ps4/msg/engus/menu.msgbnd.dcx',
                                     'dvdroot_ps4/script/talk/m21_00_00_00.talkesdbnd.dcx'])

    def test_refuses_a_wrong_folder(self):
        with tempfile.TemporaryDirectory() as out:
            run = subprocess.run([str(PYTHON), '-I', str(ROOT / 'tools/respec_mod.py'), out, out],
                                 capture_output=True, text=True)
            self.assertNotEqual(run.returncode, 0)


if __name__ == '__main__':
    unittest.main()
