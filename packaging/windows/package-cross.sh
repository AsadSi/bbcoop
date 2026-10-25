#!/usr/bin/env bash
# bbcoop: dist/bbcoop-windows.zip made on a Mac or Linux PC (package.sh runs this when it is not in MSYS2),
# from the same commit as the Mac app, so the Windows PC needs no build. The executables come from
# scripts/windows/cross-build.sh. PyInstaller cannot freeze the launcher for Windows from here, so the
# package carries python.org's Windows Python (its installer's MSIs, unpacked with msitools' msiextract:
# brew install msitools) with soulstruct for the respec mod, and Bloodborne.exe (packaging/windows/launcher.c)
# runs the launcher with it; Mrsuss60/bloodborne_pc_windows_port ships a Python the same way. Inside, as
# package.sh: Bloodborne.exe, Play Bloodborne.exe, bin/ (bb-probe.exe and its DLLs), bin/shadnet/ (the
# co-op server for hosting, with its Qt), the scripts and patches, BUILD. No game files.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
PYTHON_VERSION=3.13.9
command -v msiextract >/dev/null || { echo 'Need msiextract: brew install msitools (Linux: apt install msitools)' >&2; exit 1; }
bash scripts/windows/cross-build.sh
cross=$PWD/out/windows-cross
tools=$(echo "$cross"/llvm-mingw-*/bin)
mingw_bin=$(echo "$cross"/llvm-mingw-*/x86_64-w64-mingw32/bin)
sysroot_bin=$cross/sysroot/clang64/bin

# Windows Python with tkinter (core, exe, lib, tcltk), and soulstruct's wheels for it: the versions
# the Mac's respec Python runs.
python_dir=$cross/python-$PYTHON_VERSION
if [[ ! -f $python_dir/python.exe ]]; then
    rm -rf -- "$python_dir.new" && mkdir -p "$python_dir.new/msi"
    for part in core exe lib tcltk; do
        curl -sSLf -o "$python_dir.new/msi/$part.msi" "https://www.python.org/ftp/python/$PYTHON_VERSION/amd64/$part.msi"
        (cd "$python_dir.new" && msiextract "msi/$part.msi" > /dev/null)
    done
    rm -rf -- "$python_dir.new/msi" "$python_dir.new/libs" "$python_dir.new"/tcl/*.lib "$python_dir.new"/tcl/*.sh "$python_dir.new/tcl/nmake"
    python3 -m pip install -q --disable-pip-version-check --target "$python_dir.new/Lib/site-packages" \
        --platform win_amd64 --python-version "${PYTHON_VERSION%.*}" --implementation cp --only-binary=:all: \
        soulstruct==2.6.0 constrata==1.3.3 frozendict==2.4.7 numpy==2.5.4 rich==15.0.0 typer==0.27.3 \
        zstandard==0.25.0
    rm -rf -- "$python_dir.new/Lib/site-packages/bin"
    mv -- "$python_dir.new" "$python_dir"
fi

dest=out/stage/bbcoop-windows
rm -rf -- out/stage
mkdir -p "$dest/bin/shadnet/sqldrivers" "$dest/launcher" "$dest/tools" "$dest/licenses"
cp -R "$python_dir" "$dest/python"
"$tools/llvm-strip" -o "$dest/Bloodborne.exe" "$cross/build/bb-launch.exe"
"$tools/llvm-strip" -o "$dest/Play Bloodborne.exe" "$cross/build/bb-play.exe"
for exe in bb-probe.exe bb-gpu-capabilities.exe; do
    "$tools/llvm-strip" --strip-debug -o "$dest/bin/$exe" "$cross/build/$exe"
done
"$tools/llvm-strip" -o "$dest/bin/shadnet/shadnet.exe" "$cross/server/shadnet.exe"
cp "$cross/sysroot/clang64/share/qt6/plugins/sqldrivers/qsqlite.dll" "$dest/bin/shadnet/sqldrivers/"
cp server/worlds.cfg server/scoreboards.cfg "$dest/bin/shadnet/"

# Every DLL the executables load, followed through the DLLs' own imports: from the MSYS2 sysroot or
# the toolchain (libc++, libunwind, winpthread), else it must be Windows' own.
system_dll='^(api-ms-win-.*|ext-ms-.*|kernel32|user32|gdi32|shell32|advapi32|ws2_32|iphlpapi|crypt32|bcrypt|bcryptprimitives|dbghelp|ole32|oleaut32|shlwapi|winmm|version|secur32|ncrypt|imm32|setupapi|cfgmgr32|hid|uxtheme|dwmapi|winhttp|netapi32|userenv|mpr|ntdll|comdlg32|comctl32|rpcrt4|msvcrt|ucrtbase|usp10|dnsapi|d3d9|d3d11|d3d12|dxgi|dcomp|dwrite|d2d1|mf|mfplat|mfreadwrite|avrt|psapi|powrprof|wtsapi32|authz|wldap32|normaliz|winspool.drv|dxva2|evr|propsys|sspicli|wintrust|ksuser|mswsock|opengl32|glu32|msimg32|wsock32|gdiplus|vcruntime140|vcruntime140_1)\.dll$'
find_dll() {
    local name=$1 dir found
    for dir in "$mingw_bin" "$sysroot_bin"; do
        found=$(find "$dir" -maxdepth 1 -iname "$name" -print -quit)
        [[ -n $found ]] && { echo "$found"; return; }
    done
    return 0  # not found: Windows' own, or missing (bundle tells)
}
bundle() {  # bundle <folder> <executable>...: copies their DLLs into <folder>
    local folder=$1 queue=("${@:2}") file name dll lower missing=0
    while ((${#queue[@]})); do
        file=${queue[0]}; queue=("${queue[@]:1}")
        while read -r name; do
            [[ -f $folder/$name ]] && continue
            dll=$(find_dll "$name")
            if [[ -n $dll ]]; then
                cp "$dll" "$folder/$name"; queue+=("$folder/$name")
            else
                lower=$(tr '[:upper:]' '[:lower:]' <<<"$name")
                [[ $lower =~ $system_dll ]] || { echo "Missing DLL $name (needed by $file)" >&2; missing=1; }
            fi
        done < <("$tools/llvm-objdump" -p "$file" | sed -n 's/^ *DLL Name: //p')
    done
    return $missing
}
bundle "$dest/bin" "$dest/bin/bb-probe.exe" "$dest/bin/bb-gpu-capabilities.exe"
bundle "$dest/bin/shadnet" "$dest/bin/shadnet/shadnet.exe" "$dest/bin/shadnet/sqldrivers/qsqlite.dll"
# qsqlite.dll's own DLLs sit beside shadnet.exe, where Windows looks for them first.
for dll in "$dest"/bin/shadnet/sqldrivers/*.dll; do [[ $(basename "$dll") == qsqlite.dll ]] || mv "$dll" "$dest/bin/shadnet/"; done

cp -R scripts patches "$dest/"
cp launcher/*.py launcher/bloodborne.ico launcher/bloodborne.png "$dest/launcher/"
cp tools/respec_mod.py "$dest/tools/"
cp run.py LICENSE packaging/windows/README-Windows.txt "$dest/"
if [[ -d fsr4_shaders ]]; then cp -R fsr4_shaders "$dest/"; fi
git rev-parse --short=12 HEAD > "$dest/BUILD"  # the launchers' build check (launcher/bbcoop_online.py)
cp gpu/third_party/wepoll/LICENSE "$dest/licenses/wepoll-LICENSE.txt"
cp gpu/third_party/httplib/LICENSE "$dest/licenses/cpp-httplib-LICENSE.txt"
cp packaging/windows/licenses/*.txt "$dest/licenses/"
cp gpu/bbnet20/SHADNET-PORT.md "$dest/licenses/online-code-NOTICE.md"
cp "$python_dir/LICENSE.txt" "$dest/licenses/Python-LICENSE.txt"
for package in protobuf abseil-cpp openssl miniupnpc zlib qt6-base qt6-httpserver qt6-websockets; do
    license=$(find "$cross/sysroot/clang64/share/licenses/$package" -maxdepth 1 -type f 2>/dev/null | head -1)
    if [[ -n $license ]]; then cp "$license" "$dest/licenses/$package-$(basename "$license")"; fi
done
find "$dest" \( -name __pycache__ -o -name .DS_Store \) -prune -exec rm -r {} +
mkdir -p dist
rm -f dist/bbcoop-windows.zip
(cd out/stage && zip -qr -X ../../dist/bbcoop-windows.zip bbcoop-windows)
du -sh "$dest" dist/bbcoop-windows.zip
echo "Build $(cat "$dest/BUILD"): dist/bbcoop-windows.zip"
