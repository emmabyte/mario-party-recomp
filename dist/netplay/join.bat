@echo off
rem Joins an online session at the host's address (host.bat on their computer).
set /p MP_NETPLAY_PEER=The host's address (like 203.0.113.5, or 203.0.113.5:7845): 
echo %MP_NETPLAY_PEER% | find ":" > nul || set MP_NETPLAY_PEER=%MP_NETPLAY_PEER%:7845
set /p MP_NETPLAY_PLAYERS=How many players, the host included (2-4)? 
set /p player=Your player number (2-4, each player a different one): 
set /a MP_NETPLAY_PLAYER=player-1
set MP_NETPLAY_DELAY=3
set /p MP_NETPLAY_DELAY=Frames of delay (Enter for 3; the same as the host's): 
start "" "%~dp0MarioPartyRecompiled.exe"
