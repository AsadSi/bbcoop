#!/usr/bin/env bash
# bbcoop: build and run the C tests in an x86-64 Linux container (no GPU, no game), for machines
# that cannot build natively, such as an arm64 Mac. The Nix store is kept in the bbcoop-nix
# volume, so only the first run downloads the toolchain.
# Usage: scripts/docker-test.sh [command]   (default: bash build.sh --test)
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
command=${1:-bash build.sh --test}
exec docker run --rm --platform linux/amd64 \
    -v bbcoop-nix:/nix -v "$PWD":/src -w /src \
    -e BB_LTO=OFF -e BB_IN_NIX_SHELL=1 \
    nixos/nix sh -c "
        git config --global --add safe.directory '*'
        [ -e /nix/var/nix/profiles/per-user/root/channels/nixpkgs ] || nix-channel --update >/dev/null
        nix-shell shell.nix --run '$command'"
