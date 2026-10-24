#!/usr/bin/env bash
# bbcoop: builds dist/bbcoop-mac.zip, the Mac app for friends (Apple Silicon, macOS 13+): Bloodborne.app
# with the Tk launcher frozen by PyInstaller (it also runs the port's Python scripts, soulstruct
# included for the respec mod, so players need no Python), the port in Contents/Resources/port
# (run.sh, scripts, patches, bin/: bb-probe and its libraries, MoltenVK, the shadNet server with its Qt)
# and a BUILD file with the commit, which the launchers' build check compares. No game files.
# Unsigned (ad hoc): the first open goes through System Settings > Privacy & Security > Open Anyway.
# Run after `bash build.sh` and `cmake --build server/build-mac`. PYTHON: a Python 3.13+ with Tk
# (Homebrew: python@3.14 and python-tk@3.14); the build venv is out/pyenv-mac.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
deps=${BB_MACOS_DEPS:-$PWD/out/mac-deps}
for file in out/bb-probe out/bb-gpu-capabilities out/gpu/libbbgpu.dylib out/gpu/libbbnet20.dylib \
            server/build-mac/shadnet "$deps/lib/libMoltenVK.dylib"; do
    [[ -f $file ]] || { echo "Missing $file: bash build.sh, cmake --build server/build-mac" >&2; exit 1; }
done
python=${PYTHON:-python3.14}
if [[ ! -x out/pyenv-mac/bin/python ]]; then
    "$python" -m venv out/pyenv-mac
fi
out/pyenv-mac/bin/python -c 'import tkinter' || { echo "$python has no Tk (brew install python-tk@3.14)" >&2; exit 1; }
out/pyenv-mac/bin/python -m pip install -q --disable-pip-version-check pyinstaller pillow soulstruct==2.6.0

# The scripts run inside the launcher (--script) from their source files: every module they
# import that is not one of the port's own must be frozen in.
hidden=()
while read -r module; do hidden+=(--hidden-import "$module"); done < <(out/pyenv-mac/bin/python - <<'EOF'
import ast, importlib.util, pathlib
own = {p.stem for p in pathlib.Path('scripts').rglob('*.py')} | {'respec_mod'}
names = set()
for path in [*pathlib.Path('scripts').rglob('*.py'), pathlib.Path('tools/respec_mod.py')]:
    for node in ast.walk(ast.parse(path.read_text(encoding='utf-8'))):
        if isinstance(node, ast.Import):
            names |= {alias.name for alias in node.names}
        elif isinstance(node, ast.ImportFrom) and node.module and not node.level:
            names.add(node.module)
for name in sorted(names):
    if name.split('.')[0] not in own and importlib.util.find_spec(name.split('.')[0]):
        print(name)
EOF
)
rm -rf out/pyi-mac
out/pyenv-mac/bin/python -m PyInstaller --noconfirm --clean --log-level WARN --windowed \
    --name Bloodborne --icon "$PWD/launcher/bloodborne.png" --osx-bundle-identifier io.github.asadsi.bbcoop \
    --distpath out/pyi-mac/dist --workpath out/pyi-mac/work --specpath out/pyi-mac \
    --paths "$PWD/launcher" --paths "$PWD/scripts" --paths "$PWD/scripts/bbsc_gameserver" --collect-all soulstruct \
    "${hidden[@]}" "$PWD/launcher/bbport_launcher_win.py"
app=out/pyi-mac/dist/Bloodborne.app

# The port.
port=$app/Contents/Resources/port
mkdir -p "$port/bin/gpu" "$port/tools" "$port/launcher" "$port/licenses"
cp run.sh LICENSE "$port/"
cp -R scripts patches "$port/"
cp tools/respec_mod.py "$port/tools/"
cp launcher/bloodborne.png "$port/launcher/"
git rev-parse --short=12 HEAD > "$port/BUILD"
cp out/bb-probe out/bb-gpu-capabilities "$port/bin/"
cp out/gpu/libbbgpu.dylib out/gpu/libbbnet20.dylib "$deps/lib/libMoltenVK.dylib" "$deps/lib/libvulkan.1.dylib" \
   "$deps/lib/libSDL3.0.dylib" "$port/bin/gpu/"
printf '{"file_format_version":"1.0.0","ICD":{"library_path":"./libMoltenVK.dylib","api_version":"1.4.0"}}\n' \
    > "$port/bin/gpu/MoltenVK_icd.json"
# Only the bundle's own library folder: the build machine's paths are dropped.
for binary in "$port/bin/bb-probe" "$port/bin/bb-gpu-capabilities" "$port/bin"/gpu/*.dylib; do
    otool -l "$binary" | awk '/LC_RPATH/ {getline; getline; print $2}' | while read -r rpath; do
        [[ $rpath == @* ]] || install_name_tool -delete_rpath "$rpath" "$binary" 2> /dev/null
    done
done
otool -l "$port/bin/bb-gpu-capabilities" | grep -q '@executable_path/gpu' ||
    install_name_tool -add_rpath @executable_path/gpu "$port/bin/bb-gpu-capabilities"
# run.sh's Python (PYTHON, BB_RE_PYTHON): the launcher runs the port's scripts.
cat > "$port/bin/python3" <<'EOF'
#!/bin/sh
# bbcoop: run.sh's Python inside the Mac app: the frozen launcher runs the port's scripts.
exec "$(cd "$(dirname "$0")/../../.." && pwd)/MacOS/Bloodborne" --script "$@"
EOF
chmod +x "$port/bin/python3"
# shadNet with its Qt (macdeployqt fills an app bundle's Frameworks and PlugIns, SQLite driver included).
server=$port/bin/shadnet.app
mkdir -p "$server/Contents/MacOS"
cp server/build-mac/shadnet "$server/Contents/MacOS/"
mkdir -p "$port/server"
cp server/worlds.cfg server/scoreboards.cfg "$port/server/"  # bbcoop_online.Host copies them for shadNet
printf '<?xml version="1.0" encoding="UTF-8"?>\n<plist version="1.0"><dict><key>CFBundleExecutable</key><string>shadnet</string><key>CFBundleIdentifier</key><string>io.github.asadsi.bbcoop.shadnet</string><key>CFBundlePackageType</key><string>APPL</string></dict></plist>\n' \
    > "$server/Contents/Info.plist"
macdeployqt "$server" > /dev/null 2>&1  # its signing complaints: the app is signed below
[[ -f $server/Contents/PlugIns/sqldrivers/libqsqlite.dylib ]] || { echo 'macdeployqt left out the SQLite driver' >&2; exit 1; }
cp server/LICENSES/* "$port/licenses/" 2> /dev/null || true
find "$app" -name __pycache__ -prune -exec rm -r {} +
# Ad hoc signatures, inside out: macdeployqt rewrote copies of read-only Homebrew libraries, and a
# library whose signature no longer matches is killed when it loads.
chmod -R u+w "$app"
find "$app" -type f \( -name '*.dylib' -o -name '*.so' \) -exec codesign --force --sign - {} +
find "$app" -type d -name '*.framework' -exec codesign --force --sign - {} +
codesign --force --sign - "$server"
codesign --force --deep --sign - "$app"
codesign --verify --deep --strict "$app"

mkdir -p dist
rm -f dist/bbcoop-mac.zip
ditto -c -k --keepParent "$app" dist/bbcoop-mac.zip
du -sh "$app" dist/bbcoop-mac.zip
