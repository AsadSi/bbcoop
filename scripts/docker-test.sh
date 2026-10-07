#!/usr/bin/env bash
# bbcoop: build and run the C tests in an x86-64 Linux container (no GPU, no game), for machines
# that cannot build natively, such as an arm64 Mac. The Nix store is kept in the bbcoop-nix
# volume (bbcoop-nix-arm64 for arm64), so only the first run downloads the toolchain. Four CPUs
# keep the GPU library's compile within Docker Desktop's default memory.
# BB_DOCKER_ARCH=arm64 runs a native arm64 container instead: fast compile checks on an arm64
# Mac, but the x86-64 code (hooks, the game image) only runs under amd64.
# Usage: scripts/docker-test.sh [command]   (default: bash build.sh --test)
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
command=${1:-bash build.sh --test}
arch=${BB_DOCKER_ARCH:-amd64}
volume=bbcoop-nix; [[ $arch == amd64 ]] || volume=bbcoop-nix-$arch
exec docker run --rm --platform linux/$arch --cpuset-cpus 0-3 \
    -v $volume:/nix -v "$PWD":/src -w /src \
    -e BB_LTO=OFF -e BB_IN_NIX_SHELL=1 -e NIX_CONFIG=$'sandbox = false\nfilter-syscalls = false' \
    nixos/nix sh -c "
        git config --global --add safe.directory '*'
        mkdir -p \$HOME/.local/share
        [ -e /nix/var/nix/profiles/per-user/root/channels/nixpkgs ] || nix-channel --update >/dev/null
        nix-shell scripts/docker-shell.nix --run '$command'"
