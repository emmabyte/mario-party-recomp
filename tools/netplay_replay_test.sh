#!/usr/bin/env bash
# Netplay's determinism test (src/netplay/netplay.cpp, docs/netplay.md): records a session of the Linux build, played
# by keys sent to its window, then replays it twice, each from a new folder as the recording started, and checks that
# every frame's hash of RDRAM matches the recording's. A replay that differs stops at the first frame that does, names
# the 64 KB chunks of RDRAM that differ and writes RDRAM there to <out>/rec.mpnp.frame<n>.rdram.
#
# Usage: tools/netplay_replay_test.sh <build dir> <ROM> <out dir> [frames, default 5400 (3 minutes)]
#
# It runs headless on Xvfb, drawn by any Vulkan driver (Mesa's lavapipe will do), with no sound. The keys are the
# keyboard's defaults: Enter (Start) through the logos, the opening and the title into Mushroom Village, then Space (A)
# through Toad's messages and the warp pipe to the board Toad teaches the rules on, which four computer players play,
# with Enter now and then for the minigames' instructions screens. 54000 frames is 30 minutes.
#
# Each trip from the hub to the rules' board goes instead to a minigame of MP_NETPLAY_MINIGAMES (netplay.cpp says how),
# the same for the recording and the replays: by default a spread of kinds, the three that call frandom (Ground Pound,
# Box Mountain Mayhem, Mario Bandstand) among them, played by the four computers. Set it empty for the board alone.
set -euo pipefail

build="$(realpath "$1")"
rom="$(realpath "$2")"
out="$(realpath -m "$3")"
frames="${4:-5400}"
repo="$(cd "$(dirname "$0")/.." && pwd)"
minigames="${MP_NETPLAY_MINIGAMES-0x12:0,1,1,1;0x17:0,1,2,3;0x26:0,1,2,3;0x1E:0,1,2,3;0x1D:0,0,1,1;0x0F:0,1,1,1;0x2D:0,1,2,3;0x22:0,1,2,3;0x13:0,1,2,3;0x33:0,1,1,1;0x2E:0,1,2,3;0x0D:0,1,2,3}"
mkdir -p "$out"

display=":${NETPLAY_TEST_DISPLAY:-97}"
Xvfb "$display" -screen 0 1024x768x24 >/dev/null 2>&1 &
xvfb=$!
trap 'kill $xvfb 2>/dev/null || true' EXIT
export DISPLAY="$display" SDL_AUDIODRIVER=dummy
sleep 2

# A new folder for one run: portable, so its settings and saves stay in it, with the ROM stored as Load ROM stores it.
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

# Keys to the game's window until the program exits: Enter for the first 70 seconds, then Space, with Enter every
# eighth press, which leaves a minigame's instructions screen for the minigame.
drive() {
    local pid=$1 start=$SECONDS window="" n=0
    while kill -0 "$pid" 2>/dev/null; do
        if [ -z "$window" ]; then
            window="$(xdotool search --pid "$pid" 2>/dev/null | head -1 || true)"
            [ -n "$window" ] && xdotool windowfocus "$window" 2>/dev/null || true
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

# Starts the program in a new folder for the run named $1, with the environment given after it; its pid in $pid.
run() {
    local name=$1; shift
    local dir; dir="$(new_run "$name")"
    (cd "$dir" && exec env MP_NETPLAY_MINIGAMES="$minigames" "$@" "$build/MarioPartyRecompiled" --boot --window 640x480) > "$out/$name.log" 2>&1 < /dev/null &
    pid=$!
}

echo "recording $frames frames"
run record MP_NETPLAY_RECORD="$out/rec.mpnp" MP_NETPLAY_RECORD_FRAMES="$frames"
sleep 3
drive "$pid" &
driver=$!
wait "$pid" || true
kill $driver 2>/dev/null || true
grep '\[netplay\]' "$out/record.log" || { echo "the recording printed nothing: see $out/record.log"; exit 1; }

status=0
for n in 1 2; do
    echo "replay $n"
    run "replay$n" MP_NETPLAY_REPLAY="$out/rec.mpnp"
    if wait "$pid"; then
        echo "replay $n: every frame matched"
    else
        echo "replay $n: differed"
        status=1
    fi
    grep '\[netplay\]' "$out/replay$n.log" || true
done
exit $status
