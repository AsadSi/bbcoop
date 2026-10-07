# bbcoop: shadNet server build image (scripts/test-online.sh). Qt 6.9 for QHttpServer; the
# server README omits qt6-websockets-dev, which QHttpServer needs.
FROM ubuntu:25.10
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    git cmake ninja-build build-essential qt6-base-dev qt6-httpserver-dev qt6-websockets-dev libqt6sql6-sqlite \
    ca-certificates && rm -rf /var/lib/apt/lists/*
