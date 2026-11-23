#!/usr/bin/env bash
# Hosts an online session: you are player 1, and the others join at your address (join.sh). Your UDP port 7845 must be
# reachable from them: forward it on your router, or share a VPN such as Tailscale or ZeroTier.
cd "$(dirname "$0")"
read -rp "How many players, you included (2-4)? " players
read -rp "Frames of delay (Enter for 3; more if the game stutters over a long distance): " delay
MP_NETPLAY_HOST="$players" MP_NETPLAY_DELAY="${delay:-3}" exec ./MarioPartyRecompiled
