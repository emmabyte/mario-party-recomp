#!/usr/bin/env bash
# Hosts an online session, for testing (the launcher's Online Play does the same): you are player 1, and the others join
# at your address (join.sh). Your UDP port 7845 must be reachable from them: share a VPN or forward it on your router.
cd "$(dirname "$0")"
read -rp "How many players, you included (2-4)? " players
MP_NETPLAY_HOST="$players" exec ./MarioPartyRecompiled
