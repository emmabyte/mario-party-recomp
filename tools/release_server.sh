#!/usr/bin/env bash
# Builds the netplay server's release (src/netplay/server, dist/server/SERVER.txt): for Linux on x86-64 and on arm64
# (a Raspberry Pi, an ARM VPS), each a .tar.gz beside the game's in out/, named as the game's release is with -server.
# The server needs no ROM and holds nothing of the game, so it builds anywhere: a static binary, which runs on any Linux
# of its CPU with nothing to install. It is named after the game's release it ships with (src/main/main.cpp's
# version_string); a game whose relay protocol (relay_protocol.h's VERSION) differs from the server's is turned away.
#
#     tools/release_server.sh            # both
#     tools/release_server.sh x64        # out/MarioPartyRecompiled-v<version>-server-linux-x64.tar.gz
#     tools/release_server.sh arm64      # out/...-server-linux-arm64.tar.gz, with aarch64-linux-gnu-g++
#
# Each holds netplay-server, SERVER.txt (how to run it, as a service too, and its admin commands),
# netplay-server.service and LICENSE. tools/release.sh publish doesn't upload them: add them to the release with
# `gh release upload v<version> out/*-server-linux-*.tar.gz`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

version=$(sed -n 's/^const std::string version_string = "\(.*\)";$/\1/p' src/main/main.cpp)
[ -n "$version" ] || { echo "no version_string in src/main/main.cpp" >&2; exit 1; }
mkdir -p out

build() {
    local arch=$1 cxx=$2
    command -v "$cxx" >/dev/null || { echo "$arch: no $cxx" >&2; return 1; }
    local name="MarioPartyRecompiled-v$version-server-linux-$arch"
    local stage
    stage=$(mktemp -d)
    "$cxx" -std=c++17 -O2 -Wall -Wextra -static -s -DNETPLAY_SERVER_VERSION="\"$version\"" \
        -o "$stage/netplay-server" src/netplay/server/server.cpp
    cp dist/server/SERVER.txt dist/server/netplay-server.service LICENSE "$stage/"
    tar -C "$stage" -czf "out/$name.tar.gz" --owner=0 --group=0 \
        netplay-server SERVER.txt netplay-server.service LICENSE
    git rev-parse HEAD > "out/$name.tar.gz.commit"
    rm -rf "$stage"
    echo "out/$name.tar.gz"
}

case "${1:-all}" in
    x64) build x64 "${CXX:-g++}" ;;
    arm64) build arm64 aarch64-linux-gnu-g++ ;;
    all) build x64 "${CXX:-g++}"; build arm64 aarch64-linux-gnu-g++ ;;
    *) echo "usage: $0 [x64|arm64|all]" >&2; exit 2 ;;
esac
