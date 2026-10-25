#!/usr/bin/env bash
# Adds the launcher to the application menu (~/.local/share/applications) for this checkout.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
dest=${XDG_DATA_HOME:-$HOME/.local/share}/applications
mkdir -p "$dest"
# bbcoop: the blood moon until the launcher has a game folder; then it shows the dump's icon0.png.
sed -e "s|@LAUNCHER@|$here/bb-launcher.sh|" -e "s|@ICON@|$here/bloodborne.png|" \
    "$here/io.github.bbport.Launcher.desktop.in" \
    > "$dest/io.github.bbport.Launcher.desktop"
echo "Installed $dest/io.github.bbport.Launcher.desktop"
