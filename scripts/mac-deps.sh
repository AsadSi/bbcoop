#!/usr/bin/env bash
# bbcoop: x86-64 macOS builds of the libraries bb-probe and libbbgpu need, into out/mac-deps.
# The game runs under Rosetta 2 on Apple Silicon, so every library is built for x86_64 from
# source (Homebrew on Apple Silicon only has arm64 ones). Needs Xcode, cmake, ninja, git,
# pkg-config and Homebrew's bash (run.sh uses bash 4).
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
prefix=$PWD/out/mac-deps src=$PWD/out/mac-deps/src
mkdir -p "$src"
export MACOSX_DEPLOYMENT_TARGET=13.0
cmake_x86=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=x86_64
    -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_PREFIX_PATH="$prefix" -DBUILD_SHARED_LIBS=OFF
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON)
fetch() { # repository tag directory
    [[ -d $src/$3 ]] || git clone -q --depth 1 --branch "$2" "https://github.com/$1.git" "$src/$3"
}
cmake_dep() { # directory [cmake options...]
    local dir=$1; shift
    [[ -f $src/$dir/.installed ]] && return
    cmake -S "$src/$dir" -B "$src/$dir/build-x86" "${cmake_x86[@]}" "$@" > /dev/null
    ninja -C "$src/$dir/build-x86" install > /dev/null
    touch "$src/$dir/.installed"
}
fetch KhronosGroup/Vulkan-Headers vulkan-sdk-1.4.363.0 vulkan-headers
cmake_dep vulkan-headers
fetch KhronosGroup/Vulkan-Loader vulkan-sdk-1.4.363.0 vulkan-loader
cmake_dep vulkan-loader -DBUILD_SHARED_LIBS=ON -DVULKAN_HEADERS_INSTALL_DIR="$prefix" -DBUILD_TESTS=OFF
fetch libsdl-org/SDL release-3.4.18 sdl
cmake_dep sdl -DBUILD_SHARED_LIBS=ON -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
fetch fmtlib/fmt 12.2.0 fmt
cmake_dep fmt -DFMT_TEST=OFF -DFMT_DOC=OFF
# Zydis builds its bundled Zycore; its CMake config still looks for an installed one.
fetch zyantific/zycore-c v1.5.0 zycore
cmake_dep zycore
fetch zyantific/zydis v4.1.1 zydis
[[ -f $src/zydis/dependencies/zycore/CMakeLists.txt ]] || git -C "$src/zydis" submodule update -q --init --depth 1
cmake_dep zydis -DZYDIS_BUILD_TOOLS=OFF -DZYDIS_BUILD_EXAMPLES=OFF -DZYDIS_BUILD_DOXYGEN=OFF
fetch richgel999/miniz 3.1.2 miniz
cmake_dep miniz -DBUILD_EXAMPLES=OFF -DBUILD_TESTS=OFF -DBUILD_FUZZERS=OFF
fetch Cyan4973/xxHash v0.8.3 xxhash
cmake_dep xxhash/cmake_unofficial -DXXHASH_BUILD_XXHSUM=OFF
fetch Neargye/magic_enum v0.9.7 magic_enum
cmake_dep magic_enum -DMAGIC_ENUM_OPT_BUILD_EXAMPLES=OFF -DMAGIC_ENUM_OPT_BUILD_TESTS=OFF
fetch Tessil/robin-map v1.4.0 robin-map
cmake_dep robin-map
fetch GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator v3.3.0 vma
cmake_dep vma
fetch herumi/xbyak v7.43 xbyak
cmake_dep xbyak
# Not linked since the 0.6 port (its window overlaps the guest's direct memory from 0x7800000000,
# DARWIN_USER_MIN; the port has GUEST_MALLOC/runtime_low_malloc): kept only for the old mac branch.
# mimalloc replaces malloc in the whole process (interpose and zone): macOS's own heap is far
# above 1 TiB, and the game packs the host pointers it is given into 40 bits (on Linux glibc's
# brk heap is low). Patched to keep all its memory in [448 GiB, 512 GiB): between macOS's GPU
# carve-out and the guest's memory, above where the kernel places other mappings.
fetch microsoft/mimalloc v2.5.2 mimalloc
git -C "$src/mimalloc" apply --reverse --check "$PWD/scripts/mac/mimalloc-low.patch" 2>/dev/null ||
    git -C "$src/mimalloc" apply "$PWD/scripts/mac/mimalloc-low.patch"
cmake_dep mimalloc -DBUILD_SHARED_LIBS=ON -DMI_BUILD_STATIC=OFF -DMI_BUILD_OBJECT=OFF -DMI_BUILD_TESTS=OFF \
    -DCMAKE_C_FLAGS="-DMI_LOW_BASE=0x7000000000ull -DMI_LOW_END=0x8000000000ull"
# Boost: headers only (container, intrusive, icl, pool, asio).
if [[ ! -d $prefix/include/boost ]]; then
    curl -fsSL https://archives.boost.io/release/1.89.0/source/boost_1_89_0.tar.bz2 | tar -xj -C "$src" boost_1_89_0/boost
    mv "$src/boost_1_89_0/boost" "$prefix/include/boost"
fi
# FFmpeg (the game's movies, through avplayer): no assembly, since nasm is not needed for x86-64 C.
fetch FFmpeg/FFmpeg n8.0.1 ffmpeg
if [[ ! -f $src/ffmpeg/.installed ]]; then
    (cd "$src/ffmpeg" && ./configure --prefix="$prefix" --enable-cross-compile --arch=x86_64 --target-os=darwin \
        --cc="clang -arch x86_64" --cxx="clang++ -arch x86_64" --disable-x86asm --disable-programs --disable-doc \
        --disable-network --disable-autodetect --disable-avdevice --disable-avfilter --enable-pic > /dev/null &&
        make -j"$(sysctl -n hw.ncpu)" > /dev/null && make install > /dev/null && touch .installed)
fi
# CMake folds repeated "-framework" options into one ("-framework A B"); the linker form survives.
sed -i '' -E 's/-framework ([A-Za-z]+)/-Wl,-framework,\1/g' "$prefix/lib/pkgconfig/libavutil.pc"
# The online module (gpu/bbnet): nlohmann_json, protobuf (v21: the last without abseil; the wire
# format is the server's), miniupnpc, epoll-shim and OpenSSL. zlib is the SDK's.
fetch nlohmann/json v3.12.0 json
cmake_dep json -DJSON_BuildTests=OFF
fetch protocolbuffers/protobuf v21.12 protobuf
cmake_dep protobuf -Dprotobuf_BUILD_TESTS=OFF -Dprotobuf_WITH_ZLIB=OFF
fetch miniupnp/miniupnp miniupnpc_2_3_3 miniupnp
cmake_dep miniupnp/miniupnpc -DUPNPC_BUILD_SHARED=OFF -DUPNPC_BUILD_TESTS=OFF -DUPNPC_BUILD_SAMPLE=OFF
fetch jiixyj/epoll-shim v0.0.20240608 epoll-shim # epoll on kqueue, as shadPS4 uses on macOS
cmake_dep epoll-shim -DBUILD_TESTING=OFF
fetch openssl/openssl openssl-3.6.5 openssl
if [[ ! -f $src/openssl/.installed ]]; then
    (cd "$src/openssl" && ./Configure darwin64-x86_64-cc no-shared no-tests no-docs --prefix="$prefix" \
        --libdir=lib > /dev/null && make -j"$(sysctl -n hw.ncpu)" > /dev/null && make install_sw > /dev/null &&
        touch .installed)
fi
# MoltenVK 1.4.2 from source with the Mac port's changes (scripts/mac/moltenvk-bbport.patch: pass
# statistics, the tessellation hoist and render pass merge, vkCmdUpscaleMetalFXBB for MetalFX, opt-in
# untracked resources), x86-64, registered as an ICD. Rebuilt when the patch changes.
fetch KhronosGroup/MoltenVK v1.4.2 moltenvk-source # not "moltenvk": the release download used MoltenVK/ (case-insensitive)
mvk_patch=$PWD/scripts/mac/moltenvk-bbport.patch
if ! cmp -s "$mvk_patch" "$src/moltenvk-source/.patched"; then
    git -C "$src/moltenvk-source" checkout -q -- . && git -C "$src/moltenvk-source" clean -qfd -e build-x86
    git -C "$src/moltenvk-source" apply "$mvk_patch"
    cp "$mvk_patch" "$src/moltenvk-source/.patched"
    rm -f "$src/moltenvk-source/.installed"
fi
if [[ ! -f $src/moltenvk-source/.installed ]]; then
    cmake -S "$src/moltenvk-source" -B "$src/moltenvk-source/build-x86" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES=x86_64 -DMVK_EXCLUDE_SPIRV_TOOLS=ON -DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew > /dev/null
    ninja -C "$src/moltenvk-source/build-x86" MoltenVK > /dev/null
    cp -L "$src/moltenvk-source/build-x86/MoltenVK/libMoltenVK.dylib" "$prefix/lib/libMoltenVK.dylib"
    mkdir -p "$prefix/share/vulkan/icd.d"
    printf '{"file_format_version":"1.0.0","ICD":{"library_path":"../../../lib/libMoltenVK.dylib","api_version":"1.4.0"}}\n' \
        > "$prefix/share/vulkan/icd.d/MoltenVK_icd.json"
    touch "$src/moltenvk-source/.installed"
fi
# The Vulkan loader installs no vulkan.pc on macOS; build.sh finds Vulkan through pkg-config.
printf 'prefix=%s\nName: vulkan\nDescription: Vulkan loader\nVersion: 1.4.363\nLibs: -L${prefix}/lib -lvulkan\nCflags: -I${prefix}/include\n' \
    "$prefix" > "$prefix/lib/pkgconfig/vulkan.pc"
# The compiler build.sh uses: x86-64 code for Rosetta 2.
mkdir -p "$prefix/bin"
printf '#!/bin/sh\nexec clang -arch x86_64 "$@"\n' > "$prefix/bin/cc"
printf '#!/bin/sh\nexec clang++ -arch x86_64 "$@"\n' > "$prefix/bin/c++"
chmod +x "$prefix/bin/cc" "$prefix/bin/c++"
echo "x86-64 macOS dependencies: $prefix"
