@echo off
rem Joins an online session at the host's address (host.bat, or Online Play set to Host, on their computer), for
rem testing: the launcher's Online Play does the same. The host gives this copy its player.
set /p MP_NETPLAY_PEER=The host's address (like 26.12.34.56, or 26.12.34.56:7845): 
start "" "%~dp0MarioPartyRecompiled.exe"
