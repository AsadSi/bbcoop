#!/usr/bin/env bash
# bbcoop (from Supermedo/bloodborne_pc's package.sh): builds dist/bbcoop-windows.zip. Inside:
# Bloodborne.exe (the launcher, frozen with PyInstaller so players need no Python; it also runs
# run.py and the preparation scripts, soulstruct included for the respec mod), Play Bloodborne.exe,
# bb-probe.exe with the MSYS2 CLANG64 DLLs it loads, the scripts and patches. No game files.
# Run from an MSYS2 CLANG64 shell after `bash build.sh`. Freezing uses a Windows Python 3.13+
# (python.org; WINPYTHON overrides) and a private venv in out/pyenv.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
# bbcoop: on a Mac or Linux PC the cross-built package (no PyInstaller there).
case $(uname -s) in MINGW*|MSYS*) ;; *) exec bash packaging/windows/package-cross.sh ;; esac
source ./msys2-env.sh
[[ -f out/bb-probe.exe && -f out/bb-gpu-capabilities.exe && -f out/bb-play.exe ]] || { echo 'Build first: bash build.sh' >&2; exit 1; }

# A Windows Python (not MSYS2's) for PyInstaller.
python=${WINPYTHON:-}
if [[ -z $python ]]; then
    for candidate in /c/Python3*/python.exe "${LOCALAPPDATA:-/c/Users/$USER/AppData/Local}"/Programs/Python/Python3*/python.exe; do
        [[ -x $candidate ]] && python=$candidate
    done
fi
[[ -n $python ]] || { echo 'Need a Windows Python 3.13+ (python.org) or WINPYTHON=path\to\python.exe' >&2; exit 1; }
# Windows Python needs USERPROFILE (some MSYS2 shells start without it).
export USERPROFILE=${USERPROFILE:-$(cygpath -w "/c/Users/$(id -un)")}
if [[ ! -x out/pyenv/Scripts/python.exe ]]; then
    "$python" -m venv out/pyenv
fi
out/pyenv/Scripts/python.exe -m pip install -q --disable-pip-version-check pyinstaller soulstruct==2.6.0
# The scripts run inside Bloodborne.exe (--script): their modules come along.
hidden=()
for module in argparse base64 bisect collections contextlib datetime hashlib json re secrets shutil socket \
              stat struct tempfile traceback uuid zlib xml.etree.ElementTree http.server urllib.error \
              urllib.request ctypes.wintypes link_libc import_scopes game_check bbhost_service; do
    hidden+=(--hidden-import "$module")
done
out/pyenv/Scripts/python.exe -m PyInstaller --noconfirm --clean --log-level WARN --windowed \
    --name Bloodborne --icon "$(cygpath -w "$PWD/launcher/bloodborne.ico")" --distpath out/pyi-dist \
    --workpath out/pyi-work --specpath out/pyi-work --paths "$(cygpath -w "$PWD/scripts")" \
    --paths "$(cygpath -w "$PWD/scripts/bbsc_gameserver")" --collect-all soulstruct "${hidden[@]}" \
    "$(cygpath -w "$PWD/launcher/bbport_launcher_win.py")"

dest=out/stage/bbcoop-windows
rm -rf -- out/stage
mkdir -p "$dest/bin" "$dest/launcher" "$dest/tools" "$dest/licenses"
cp -r out/pyi-dist/Bloodborne/. "$dest/"
llvm-strip -o "$dest/Play Bloodborne.exe" out/bb-play.exe
cp launcher/bloodborne.ico launcher/bloodborne.png "$dest/launcher/"
# The executables without debug information (out/ keeps the symbols for crash reports).
for exe in bb-probe.exe bb-gpu-capabilities.exe; do
    llvm-strip --strip-debug -o "$dest/bin/$exe" "out/$exe"
done
# Every DLL the executables load from the CLANG64 tree (SDL3, FFmpeg, Vulkan loader, ...).
ldd "$dest/bin/bb-probe.exe" "$dest/bin/bb-gpu-capabilities.exe" |
    awk '/\/clang64\/bin\// {print $3}' | sort -u | while read -r dll; do
        cp -u "$dll" "$dest/bin/"
    done
cp -r scripts patches "$dest/"
cp tools/respec_mod.py "$dest/tools/"
cp run.py LICENSE packaging/windows/README-Windows.txt "$dest/"
if [[ -d fsr4_shaders ]]; then cp -r fsr4_shaders "$dest/"; fi
# The libraries compiled in, the MSYS2 ones it loads, and where the online code comes from.
cp gpu/third_party/wepoll/LICENSE "$dest/licenses/wepoll-LICENSE.txt"
cp gpu/third_party/httplib/LICENSE "$dest/licenses/cpp-httplib-LICENSE.txt"
cp packaging/windows/licenses/*.txt "$dest/licenses/"
cp gpu/bbnet20/SHADNET-PORT.md "$dest/licenses/online-code-NOTICE.md"
for package in protobuf abseil-cpp openssl miniupnpc zlib; do
    license=/clang64/share/licenses/$package/LICENSE
    if [[ -f $license ]]; then cp "$license" "$dest/licenses/$package-LICENSE.txt"; fi
done
find "$dest" -name __pycache__ -prune -exec rm -r {} +
mkdir -p dist
rm -f dist/bbcoop-windows.zip
(cd out/stage && powershell -NoProfile -Command \
    "Compress-Archive -Path bbcoop-windows -DestinationPath ../../dist/bbcoop-windows.zip")
du -sh "$dest" dist/bbcoop-windows.zip
