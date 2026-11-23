#!/usr/bin/env bash
# Joins an online session at the host's address (host.sh on their computer).
cd "$(dirname "$0")"
read -rp "The host's address (like 203.0.113.5, or 203.0.113.5:7845): " peer
read -rp "How many players, the host included (2-4)? " players
read -rp "Your player number (2-4, each player a different one): " player
read -rp "Frames of delay (Enter for 3; the same as the host's): " delay
case "$peer" in *:*) ;; *) peer="$peer:7845" ;; esac
MP_NETPLAY_PEER="$peer" MP_NETPLAY_PLAYERS="$players" MP_NETPLAY_PLAYER=$((player - 1)) MP_NETPLAY_DELAY="${delay:-3}" \
    exec ./MarioPartyRecompiled
