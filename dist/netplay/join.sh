#!/usr/bin/env bash
# Joins an online session at the host's address (host.sh, or Online Play set to Host, on their computer), for testing:
# the launcher's Online Play does the same. The host gives this copy its player.
cd "$(dirname "$0")"
read -rp "The host's address (like 26.12.34.56, or 26.12.34.56:7845): " peer
MP_NETPLAY_PEER="$peer" exec ./MarioPartyRecompiled
