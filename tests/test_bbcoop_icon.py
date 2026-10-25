"""bbcoop: the launchers' icon from the player's dump (launcher/bbcoop_icon.py, no GTK or Tk)."""
from paths import ROOT
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(ROOT / 'launcher'))
import bbcoop_icon  # noqa: E402


def png(width, height):
    """A valid PNG of one grey colour."""
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    rows = b''.join(b'\0' + b'\x80' * width for _ in range(height))
    return (bbcoop_icon.PNG_MAGIC + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 0, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


class IconTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.dir = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def test_dump_icon(self):
        fallback = self.dir / 'bloodborne.png'
        self.assertIsNone(bbcoop_icon.dump_icon(''))
        self.assertIsNone(bbcoop_icon.dump_icon(self.dir))  # a folder without sce_sys/icon0.png
        self.assertEqual(bbcoop_icon.icon_for('', fallback), fallback)
        icon = self.dir / 'sce_sys' / 'icon0.png'
        icon.parent.mkdir()
        icon.write_bytes(png(4, 4))
        self.assertEqual(bbcoop_icon.dump_icon(str(self.dir)), icon)
        self.assertEqual(bbcoop_icon.icon_for(self.dir, fallback), icon)

    def test_ico_from_png(self):
        data = png(256, 256)
        ico = bbcoop_icon.ico_from_png(data)
        reserved, kind, count = struct.unpack('<HHH', ico[:6])
        width, height, _colours, _r, planes, bits, size, offset = struct.unpack('<BBBBHHII', ico[6:22])
        self.assertEqual((reserved, kind, count), (0, 1, 1))
        self.assertEqual((width, height, planes, bits), (0, 0, 1, 32))  # 256 is written as 0
        self.assertEqual((size, offset), (len(data), 22))
        self.assertEqual(ico[22:], data)
        self.assertEqual(struct.unpack('<BB', bbcoop_icon.ico_from_png(png(48, 32))[6:8]), (48, 32))
        with self.assertRaises(ValueError):
            bbcoop_icon.ico_from_png(png(512, 512))
        with self.assertRaises(ValueError):
            bbcoop_icon.ico_from_png(b'GIF89a' + bytes(30))

    def test_menu_entry_needs_an_entry_or_an_appimage(self):
        entry = self.dir / 'io.github.bbport.Launcher.desktop'
        self.assertFalse(bbcoop_icon.update_menu_entry('/icon.png', entry))
        self.assertFalse(entry.exists())

    def test_menu_entry_for_an_appimage(self):
        entry = self.dir / 'apps' / 'io.github.bbport.Launcher.desktop'
        self.assertTrue(bbcoop_icon.update_menu_entry('/g/sce_sys/icon0.png', entry,
                                                      appimage='/home/x/My Games/Bloodborne.AppImage'))
        text = entry.read_text()
        self.assertIn('Exec="/home/x/My Games/Bloodborne.AppImage"\n', text)
        self.assertIn('Icon=/g/sce_sys/icon0.png\n', text)
        self.assertTrue(text.startswith('[Desktop Entry]\n'))

    def test_menu_entry_icon_is_replaced_in_place(self):
        entry = self.dir / 'io.github.bbport.Launcher.desktop'
        entry.write_text('[Desktop Entry]\nType=Application\nName=Bloodborne\nExec=/x/bb-launcher.sh\n'
                         'Icon=/x/bloodborne.png\nCategories=Game;\n\n[Desktop Action offline]\n'
                         'Name=Play offline\nIcon=keep-me\n')
        self.assertTrue(bbcoop_icon.update_menu_entry('/g/sce_sys/icon0.png', entry, appimage='/other'))
        text = entry.read_text()
        self.assertIn('Icon=/g/sce_sys/icon0.png\nCategories=Game;\n', text)
        self.assertIn('Exec=/x/bb-launcher.sh\n', text)  # an existing entry keeps its Exec
        self.assertIn('Icon=keep-me\n', text)  # other groups are left alone
        self.assertFalse(bbcoop_icon.update_menu_entry('/g/sce_sys/icon0.png', entry))  # unchanged

    def test_menu_entry_without_an_icon_key(self):
        entry = self.dir / 'io.github.bbport.Launcher.desktop'
        entry.write_text('[Desktop Entry]\nName=Bloodborne\n')
        self.assertTrue(bbcoop_icon.update_menu_entry('/i.png', entry))
        self.assertEqual(entry.read_text(), '[Desktop Entry]\nIcon=/i.png\nName=Bloodborne\n')

    def test_exec_quote(self):
        self.assertEqual(bbcoop_icon.exec_quote('/a/b.AppImage'), '/a/b.AppImage')
        self.assertEqual(bbcoop_icon.exec_quote('/a b/$x"y'), '"/a b/\\$x\\"y"')


if __name__ == '__main__':
    unittest.main()
