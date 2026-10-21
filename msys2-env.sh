# bbcoop (from Supermedo/bloodborne_pc): sourced by the Windows build and packaging scripts. Puts the
# MSYS2 CLANG64 toolchain first on PATH. MSYS2_ROOT overrides the default C:/msys64.
msys2_root=${MSYS2_ROOT:-/c/msys64}
if [[ -d $msys2_root/clang64/bin ]]; then
    export PATH="$msys2_root/clang64/bin:$msys2_root/usr/bin:$PATH"
    export MSYSTEM=CLANG64
fi
