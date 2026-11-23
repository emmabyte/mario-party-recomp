#!/usr/bin/env bash
# Netplay's session test (src/netplay, docs/netplay.md): two copies of the Linux build on one computer,
# each a player of one session over UDP on localhost, which compare their hashes of RDRAM as they play. Player 1 is
# played by keys sent to its window, as tools/netplay_replay_test.sh plays its recording, and player 2 by no one. Each
# copy ends after the frames given and prints how many hashes it compared; a frame whose hashes differ ends both and
# names it.
#
# Usage: tools/netplay_session_test.sh <build dir> <ROM> <out dir> [frames, default 5400 (3 minutes)]
#
# MP_NETPLAY_DELAY (default 3) and MP_NETPLAY_DROP (a share of packets each copy drops, in percent; default 0) are
# passed on; MP_NETPLAY_MINIGAMES as in the replay test.
set -euo pipefail

build="$(realpath "$1")"
rom="$(realpath "$2")"
out="$(realpath -m "$3")"
frames="${4:-5400}"
repo="$(cd "$(dirname "$0")/.." && pwd)"
minigames="${MP_NETPLAY_MINIGAMES-0x12:0,1,1,1;0x17:0,1,2,3;0x26:0,1,2,3;0x1E:0,1,2,3}"
mkdir -p "$out"

display=":${NETPLAY_TEST_DISPLAY:-96}"
Xvfb "$display" -screen 0 1400x768x24 >/dev/null 2>&1 &
xvfb=$!
trap 'kill $xvfb 2>/dev/null || true' EXIT
export DISPLAY="$display" SDL_AUDIODRIVER=dummy
sleep 2

new_run() {
    local dir="$out/$1"
    rm -rf "$dir"
    mkdir -p "$dir"
    touch "$dir/portable.txt"
    cp "$rom" "$dir/marioparty.n64.us.1.0.z64"
    ln -s "$repo/assets" "$dir/assets"
    ln -s "$repo/recompcontrollerdb.txt" "$dir/recompcontrollerdb.txt"
    echo "$dir"
}

# The game's own window of a copy (each has a hidden one of SDL's too); empty until it is up.
game_window() {
    xdotool search --pid "$1" --name 'Mario Party' 2>/dev/null | tail -1 || true
}

# Keys to player 1's window, as the replay test sends them, with player 2's moved aside: with no window manager the
# keys go to the window under the pointer, so the pointer sits on player 1's.
drive() {
    local pid=$1 start=$SECONDS window="" other="" n=0
    while kill -0 "$pid" 2>/dev/null; do
        if [ -z "$window" ]; then
            window="$(game_window "$pid")"
            other="$(game_window "$p2")"
            [ -n "$other" ] && xdotool windowmove "$other" 700 0 2>/dev/null || true
        fi
        if [ -n "$window" ]; then
            xdotool windowmove "$window" 0 0 windowraise "$window" mousemove --window "$window" 100 100 \
                windowfocus "$window" 2>/dev/null || true
        fi
        if [ $((SECONDS - start)) -lt 70 ]; then
            xdotool key --delay 120 Return 2>/dev/null || true
            sleep 2.5
        else
            n=$((n + 1))
            if [ $((n % 8)) -eq 0 ]; then
                xdotool key --delay 120 Return 2>/dev/null || true
            else
                xdotool key --delay 120 space 2>/dev/null || true
            fi
            sleep 1.5
        fi
    done
}

run() {
    local name=$1; shift
    local dir; dir="$(new_run "$name")"
    (cd "$dir" && exec env MP_NETPLAY_MINIGAMES="$minigames" MP_NETPLAY_FRAMES="$frames" \
        MP_NETPLAY_DELAY="${MP_NETPLAY_DELAY:-3}" MP_NETPLAY_DROP="${MP_NETPLAY_DROP:-0}" "$@" \
        "$build/MarioPartyRecompiled" --boot --window 640x480) > "$out/$name.log" 2>&1 < /dev/null &
}

echo "a session of $frames frames"
run player1 MP_NETPLAY_PLAYER=0 MP_NETPLAY_PORT=7845 MP_NETPLAY_PEER=127.0.0.1:7846
p1=$!
run player2 MP_NETPLAY_PLAYER=1 MP_NETPLAY_PORT=7846 MP_NETPLAY_PEER=127.0.0.1:7845
p2=$!
sleep 3
drive "$p1" &
driver=$!
status=0
wait "$p1" || status=1
wait "$p2" || status=1
kill $driver 2>/dev/null || true
for name in player1 player2; do
    echo "$name:"
    grep '\[netplay\]' "$out/$name.log" | grep -v ': overlay\|asking for' || true
done
exit $status
