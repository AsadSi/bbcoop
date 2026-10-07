#!/usr/bin/env bash
# bbcoop: builds the co-op server (server/) for hosting from the launcher: server/build-linux/shadnet.
# Needs CMake, Ninja, a C++17 compiler and Qt 6.9+ with HttpServer, WebSockets and the SQLite SQL
# driver, e.g. Ubuntu 25.10: qt6-base-dev qt6-httpserver-dev qt6-websockets-dev libqt6sql6-sqlite;
# Arch: qt6-base qt6-httpserver qt6-websockets. Without them, BB_SERVER_DOCKER=1 builds in the
# Ubuntu container from scripts/server.Dockerfile instead.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
git submodule update --init --depth 1 server/externals/protobuf
if [[ ${BB_SERVER_DOCKER:-} == 1 ]]; then
    docker build -q -t bbcoop-server-build -f scripts/server.Dockerfile scripts >/dev/null
    exec docker run --rm -u "$(id -u):$(id -g)" -v "$PWD/server":/src -w /src bbcoop-server-build \
        sh -ec 'cmake -G Ninja -B build-linux -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build build-linux'
fi
cmake -G Ninja -S server -B server/build-linux -DCMAKE_BUILD_TYPE=Release
cmake --build server/build-linux
echo "Built $PWD/server/build-linux/shadnet"
