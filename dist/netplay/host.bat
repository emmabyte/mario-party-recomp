@echo off
rem Hosts an online session: you are player 1, and the others join at your address (join.bat). Your UDP port 7845
rem must be reachable from them: forward it on your router, or share a VPN such as Tailscale or ZeroTier.
set /p MP_NETPLAY_HOST=How many players, you included (2-4)? 
start "" "%~dp0MarioPartyRecompiled.exe"
