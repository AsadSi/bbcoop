"""bbcoop: the launchers show the player's own game icon (sce_sys/icon0.png of their dump), the
blood moon (launcher/bloodborne.png, passed in by the launcher: a frozen app keeps it next to the
executable) until a game folder is chosen (Asad, 2026-10-25). No GTK or Tk: both launchers and
the tests use it. The game's icon is never copied into a package; each player's comes from their dump."""
import os
import struct
from pathlib import Path

APP_ID = "io.github.bbport.Launcher"  # the GTK launcher's application id and its menu entry's name
PNG_MAGIC = b"\x89PNG\r\n\x1a\n"


def dump_icon(game_dir):
    """The dump's icon0.png, or None without a game folder or the file."""
    if not game_dir:
        return None
    icon = Path(game_dir).expanduser() / "sce_sys" / "icon0.png"
    return icon if icon.is_file() else None


def icon_for(game_dir, fallback):
    return dump_icon(game_dir) or Path(fallback)


def png_size(data):
    """(width, height) from a PNG's IHDR; raises ValueError for anything else."""
    if data[:8] != PNG_MAGIC or data[12:16] != b"IHDR":
        raise ValueError("not a PNG")
    return struct.unpack(">II", data[16:24])


def ico_from_png(data):
    """An .ico holding one PNG image of at most 256 x 256 (Windows Vista and later read these)."""
    width, height = png_size(data)
    if width > 256 or height > 256:
        raise ValueError(f"{width}x{height} is over the 256x256 an .ico holds")
    # ICONDIR (reserved, type 1 = icon, 1 image), then its entry: 256 is written as 0.
    entry = struct.pack("<BBBBHHII", width % 256, height % 256, 0, 0, 1, 32, len(data), 6 + 16)
    return struct.pack("<HHH", 0, 1, 1) + entry + data


def menu_entry():
    """The GTK launcher's menu entry (launcher/install-desktop.sh writes it for a checkout)."""
    data = Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local" / "share")
    return data / "applications" / f"{APP_ID}.desktop"


def exec_quote(argument):
    """One argument of a desktop entry's Exec key (Desktop Entry Specification, "The Exec key")."""
    if not any(c in argument for c in ' \t\n"\'\\><~|&;$*?#()`'):
        return argument
    return '"' + "".join("\\" + c if c in '"`$\\' else c for c in argument) + '"'


def update_menu_entry(icon, entry=None, appimage=None):
    """Points the menu entry's Icon at `icon`. Without an entry, an AppImage (its path) gets one.
    True when the file was written."""
    entry = Path(entry) if entry else menu_entry()
    if not entry.is_file():
        if not appimage:
            return False
        lines = ["[Desktop Entry]", "Type=Application", "Name=Bloodborne",
                 "Comment=Bloodborne (bbcoop): Host, Join and Play", f"Exec={exec_quote(str(appimage))}",
                 f"Icon={icon}", "Terminal=false", "Categories=Game;"]
    else:
        lines = entry.read_text(encoding="utf-8").splitlines()
        group, at = None, None
        for number, line in enumerate(lines):
            if line.startswith("["):
                group = line.strip()
                if group == "[Desktop Entry]":
                    at = number + 1
            elif group == "[Desktop Entry]" and line.split("=", 1)[0].strip() == "Icon":
                if line.split("=", 1)[1].strip() == str(icon):
                    return False
                lines[number], at = f"Icon={icon}", None
                break
        if at is not None:  # the group has no Icon key yet
            lines.insert(at, f"Icon={icon}")
    entry.parent.mkdir(parents=True, exist_ok=True)
    temporary = entry.with_name(entry.name + ".tmp")
    temporary.write_text("\n".join(lines) + "\n", encoding="utf-8")
    temporary.replace(entry)
    return True
