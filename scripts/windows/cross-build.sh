#!/usr/bin/env bash
# bbcoop: builds the Windows executables on a Mac or Linux PC, to catch Windows compile and link errors
# without a Windows machine. llvm-mingw (clang, libc++, UCRT: what MSYS2 CLANG64 uses) against the MSYS2
# CLANG64 packages the Windows build installs (docs/bbcoop/windows.md), unpacked into
# out/windows-cross/sysroot. protoc runs on this machine, at the sysroot's protobuf version.
# The executables are for checking: players build on Windows (build.sh) or get the package.
# Usage: bash scripts/windows/cross-build.sh   (output: out/windows-cross/build/bb-probe.exe, ...)
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
LLVM_MINGW=${LLVM_MINGW:-20261020}
dir=$PWD/out/windows-cross
mkdir -p "$dir/pkgs"
case $(uname -s) in
Darwin) host=macos-universal; protoc_host=osx-universal_binary ;;
*) host=ubuntu-22.04-x86_64; protoc_host=linux-x86_64 ;;
esac
toolchain=$dir/llvm-mingw-$LLVM_MINGW-ucrt-$host
if [[ ! -d $toolchain ]]; then
    curl -sSLf "https://github.com/mstorsjo/llvm-mingw/releases/download/$LLVM_MINGW/llvm-mingw-$LLVM_MINGW-ucrt-$host.tar.xz" |
        tar -xJ -C "$dir"
fi
python=$(command -v python3.12 || command -v python3)
if [[ ! -f $dir/pkgs/clang64.db ]]; then
    curl -sSLf -o "$dir/pkgs/clang64.db" https://repo.msys2.org/mingw/clang64/clang64.db
fi
if [[ ! -d $dir/sysroot/clang64 ]]; then
    "$python" -I scripts/windows/msys2_fetch.py "$dir/pkgs" "$dir/sysroot" vulkan-headers vulkan-loader \
        vulkan-memory-allocator sdl3 fmt boost robin-map xxhash ffmpeg zydis spirv-tools spirv-headers protobuf \
        openssl zlib miniupnpc nlohmann-json | tail -1
fi
# The sysroot's protoc.exe cannot run here: one of this machine at the same version stands in for it.
version=$(ls "$dir/pkgs" | sed -n 's/^mingw-w64-clang-x86_64-protobuf-\([0-9.]*\)-.*/\1/p' | head -1)
if [[ ! -x $dir/protoc/bin/protoc ]]; then
    curl -sSLf -o "$dir/protoc.zip" "https://github.com/protocolbuffers/protobuf/releases/download/v$version/protoc-$version-$protoc_host.zip"
    unzip -qo "$dir/protoc.zip" -d "$dir/protoc"
fi
printf '#!/bin/sh\nexec "%s" "$@"\n' "$dir/protoc/bin/protoc" > "$dir/sysroot/clang64/bin/protoc.exe"
chmod +x "$dir/sysroot/clang64/bin/protoc.exe"
cat > "$dir/toolchain.cmake" <<CMAKE
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER $toolchain/bin/x86_64-w64-mingw32-clang)
set(CMAKE_CXX_COMPILER $toolchain/bin/x86_64-w64-mingw32-clang++)
set(CMAKE_RC_COMPILER $toolchain/bin/x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH $dir/sysroot/clang64)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
CMAKE
export PKG_CONFIG_LIBDIR=$dir/sysroot/clang64/lib/pkgconfig:$dir/sysroot/clang64/share/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=$dir/sysroot
cmake -S gpu -B "$dir/build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$dir/toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBB_PGO=off -DBB_LTO=OFF > "$dir/cmake.log" || { tail -30 "$dir/cmake.log"; exit 1; }
grep '^-- Online module' "$dir/cmake.log" || true
if ! ninja -C "$dir/build" bb-probe bb-gpu-capabilities bb-play > "$dir/build.log" 2>&1; then
    grep -E 'error:|undefined symbol|duplicate symbol' "$dir/build.log" | head -40 >&2
    echo "Windows cross build failed (full log: $dir/build.log)" >&2; exit 1
fi
echo "Built $dir/build/bb-probe.exe (Windows, for checking)"
