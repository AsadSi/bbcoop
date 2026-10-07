#!/usr/bin/env bash
# bbcoop: end-to-end online test without the game. Builds the shadNet server (from
# ${SHADNET_SRC:-../shadnet-p2p-ref}) in a container, registers two accounts, then runs
# tests/online_client.cpp as host and guest in x86-64 containers on a private Docker network:
# sign-in, room create/join, NP signaling and P2P datagrams both ways. Needs `bash build.sh`
# (scripts/docker-test.sh) to have built out/ first.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
server_src=$(cd "${SHADNET_SRC:-../shadnet-p2p-ref}" && pwd)
nix_config=$'sandbox = false\nfilter-syscalls = false'

docker build -q -t bbcoop-server-build -f scripts/server.Dockerfile scripts >/dev/null
if [[ ! -x $server_src/build-linux/shadnet || ! -x $server_src/build-sample/shadnet-sample ]]; then
    (cd "$server_src" && git submodule update --init --recursive --depth 1)
    docker run --rm -v "$server_src":/src -w /src bbcoop-server-build sh -ec '
        cmake -G Ninja -B build-linux -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build build-linux >/dev/null
        cmake -G Ninja -S clientsample -B build-sample -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build build-sample >/dev/null'
fi

scripts/docker-test.sh '
    mkdir -p out/client-objs
    for f in src/runtime*.c; do gcc -std=c11 -O2 -g -pthread -I. -Isrc $(pkg-config --cflags vulkan sdl3) -c $f -o out/client-objs/$(basename $f .c).o; done
    g++ -std=c++20 -O1 -g -Wall -Werror -Wno-unused-function -pthread -I. -Isrc -Igpu/shim -Igpu/shadps4 tests/online_client.cpp out/client-objs/*.o out/libatrac9.a -lm \
        -Lout/gpu -lbbgpu -Wl,-rpath,$PWD/out/gpu -rdynamic $(pkg-config --libs vulkan sdl3 fmt) -o out/online-client' >/dev/null

cleanup() { docker rm -f bbcoop-server bbcoop-host bbcoop-guest >/dev/null 2>&1 || true; docker network rm bbcoop-test >/dev/null 2>&1 || true; }
trap cleanup EXIT
cleanup
docker network create bbcoop-test >/dev/null
rm -rf out/online-server && mkdir -p out/online-server && cp "$server_src"/build-linux/{shadnet,worlds.cfg,scoreboards.cfg} out/online-server/
docker run -d --name bbcoop-server --network bbcoop-test -v "$PWD/out/online-server":/srv -w /srv bbcoop-server-build \
    sh -c 'timeout 5 ./shadnet >/dev/null 2>&1; sed -i "s/^Host=.*/Host=0.0.0.0/" shadnet.cfg; exec ./shadnet' >/dev/null
until docker logs bbcoop-server 2>&1 | grep -q WebApiServer; do sleep 1; done
server=$(docker inspect -f '{{(index .NetworkSettings.Networks "bbcoop-test").IPAddress}}' bbcoop-server)
for who in hosthunter guesthunter; do
    docker run --rm --network bbcoop-test -v "$server_src":/src bbcoop-server-build \
        /src/build-sample/shadnet-sample "$server" 31313 register $who testpass123 $who@example.invalid >/dev/null
done

client() {
    docker run --rm --platform linux/amd64 --network bbcoop-test --name bbcoop-$1 -v bbcoop-nix:/nix -v "$PWD":/src -w /src \
        -e BB_SHADNET_SERVER=$server:31313 -e BB_SHADNET_NPID=$1hunter -e BB_SHADNET_PASSWORD=testpass123 \
        -e NIX_CONFIG="$nix_config" nixos/nix sh -c "mkdir -p \$HOME/.local/share; nix-shell scripts/docker-shell.nix --run \
        'timeout 240 out/online-client $1 out/online-server/room.txt'" 2>&1 | grep -v '^copying\|^unpacking\|/nix/store' > out/online-$1.log
}
client host & host_pid=$!
sleep 20
client guest & guest_pid=$!
status=0
wait $host_pid || status=1
wait $guest_pid || status=1
for role in host guest; do echo "--- $role"; tail -6 out/online-$role.log; done
grep -q '^PASS' out/online-host.log && grep -q '^PASS' out/online-guest.log && [[ $status == 0 ]] && echo "online test: PASS" || { echo "online test: FAIL"; exit 1; }
