# Netplay

Online play is input lockstep: each player runs the game on their own computer, only the controllers' inputs travel,
and every copy of the game reads the same four controllers on the same frame. That only works if the game turns the
same inputs into the same RDRAM, frame for frame, so the first part here is the test of that: a recording of a
session's inputs and RDRAM hashes, and its replay.

The code is in two parts:

- `src/netplay/core`, which knows no game: record and replay, the lockstep session over UDP with its handshake, and
  the hash of the game state. A game gives it three things (`netplay_core.h`'s `Game`): how many pad samples a frame
  reads, the ranges of RDRAM that hold its state, and its identity, the lines two copies must share to play alike.
  `netplay_core.cpp`'s header lists the environment variables, and `session.cpp`'s says how a session goes.
- `src/netplay/mario_party.cpp`, Mario Party's side: the pad ring, the clocks and threads below, the state's ranges
  and the identity (the ROM, the build and its commit, the mods enabled, Board Movement and Map Scrolling).

The `MP_NETPLAY` CMake option (on by default) builds it; off, `src/netplay/off.cpp` answers us.toml's hooks as they
answer when nothing runs. Built in, nothing runs until one of the environment variables starts a recording, a replay
or a session.

## Where the game is fed

- The main loop (at 0x8003B52C) runs one logic frame every two VIs: `func_80013E84`, which drains the pad ring into
  `ContBtn`, `ContBtnTrg`, `ContStkX/Y` and the stick history, then `HuPrcCall(1)`. The scheduler's tick is always
  1, so a slow frame never ticks a process's sleep twice.
- During a session the ring is filled by netplay, two entries a frame, once a frame, at `func_80013E84`'s start
  (us.toml's hook), and the controller thread's reads at the VI go to netplay's queue instead (the hook at
  `func_80013974`'s start). Without a session both hooks return at once.
- `rand8`, the game's random number generator (`src/engine/ovlman.c`), is an LCG in RDRAM, so it repeats with the
  inputs.
- The game's logic also reads three clocks that run in real time, which no two computers share. During a session each
  runs by the frame instead (us.toml's hooks, `src/netplay/mario_party.cpp`'s header):
  - the VI count (`D_800F383C`), which the Mini-Game House, the press Start screen and the sound effects' timers read,
    is two a frame; the main loop and the swap chain, which wait on it, read the VIs that really passed;
  - the count of frames drawn (`D_800F3758`) is the frame's number;
  - the CPU's count register, which five copies of Mario Party 4's `frandom` seed themselves from (Box Mountain
    Mayhem, Ground Pound, Mario Bandstand, an unused minigame, the staff roll), reads a count from the frame.
- Three of the game's threads answer it in real time, and the logic waits on them. A process that asks the SI manager
  (the EEPROM, the Rumble Pak) for something without blocking sleeps a frame at a time until the answer comes: during a
  session it waits for it there and then. The sound driver's thread moves the music's and the sounds' state on as it
  makes sound at each retrace, and the logic reads that back (omMain won't change scenes until the music has stopped,
  objmain copies the music's state into its own flags): during a session the thread gets two retraces from netplay at
  the start of each frame, rather than from the VI handler, makes 544, 528, 528 samples in turn rather than as many as
  the sound card's queue asks for, and the frame starts once it has made them. A retrace that starts a tune gives the
  music player its bank a retrace later, which on a console comes as the frame's logic runs, before a minigame that
  reads the tune (Mario Bandstand) gets to it: during a session that frame's retraces go on until the player has the
  bank, and the frames after it have as many fewer.
  A board's background is loaded a piece at a time by two threads (`src/48D90.c`), and the board takes the finished
  pieces each frame without waiting: during a session each piece the board sends them is finished before it goes on.
- At each frame's start the heaps' free blocks are zeroed: a block keeps what was last written to it, some of that was
  written as real time passed (the sound library, called from a process, leaves what it read on the process's stack),
  and a block allocated again starts with it.
- The hash leaves out what the runtime and the game's system threads write as real time passes and the logic never
  reads back: threads and their stacks, the scheduler's and the swap chain's queues, the framebuffers' order, the
  performance meter, the sound driver's data and heap, the Rumble Paks' state, the stacks of the processes that are
  alive, below where each sleeps being what was left there, and the display lists the logic writes for each frame,
  which the game only draws (a wipe puts in them the address of the frame buffer the swap chain chose as real time
  passed) (`mario_party.cpp`'s `excluded` and `state_ranges`).
- During a replay the ports with a controller are the recording's (`get_connected_device_info`, `src/main/main.cpp`).

## Playing a session

Two to four players, each on their own computer with the same ROM, the same build, the same mods enabled and the same
Board Movement and Map Scrolling. One hosts; the others join the host. The launcher's **Online Play** opens its
screens (`src/netplay/online_screens.cpp`): Host a Party, or Join a Party at the host's address, then the party, a
lobby where the handshake runs before the game starts (`netplay::lobby_open`, `core/session.cpp`). It shows each
player as they join, with their name, their round trip to the host and whether they're ready; once everyone is, the
host's Let's Party! settles how many play and starts the game everywhere; Leave leaves it. The settings' Online tab
(`src/netplay/online_tab.cpp`) keeps the name, the host's port and the address last joined. The environment starts one too,
for the tests (`src/netplay/core/netplay_core.cpp` lists the variables), and a release carries `dist/netplay`'s
`host` and `join` scripts (`.bat` on Windows, `.sh` on Linux) that set it, and `ONLINE.txt`, the players' how-to.

```sh
# The host, player 1, for three players:
MP_NETPLAY_HOST=3 ./MarioPartyRecompiled
# Each other player, by the host's address; the host gives each its player in the order they join:
MP_NETPLAY_PEER=<host's address> ./MarioPartyRecompiled
```

The handshake runs at the game's entry, before it reads its save (`mp_netplay::start`, from `src/main/main.cpp`):

- Each guest's hello reaches the host, which gives it its player and says how many play.
- The copies compare what makes them play alike (the identity: ROM, build, mods, options); a copy that differs says
  what differs and quits.
- The hellos carry clocks, and the host chooses the delay from the longest round trip it measures: a guest's frame
  waits for its inputs to reach the host and the whole frame to come back, so the delay covers a round trip and some
  for its jitter (`MP_NETPLAY_DELAY` on the host sets it instead). As the session plays the host keeps measuring, and
  the delay goes up at once when the connection gets worse and down a frame at a time after ten seconds in which it
  could have, since a steady delay is easier to play with than one that keeps changing; `session.log` names each
  change. `MP_NETPLAY_LATENCY_MS` (`session.cpp`) holds back each packet a copy sends, to test this on one computer.
- Every copy plays from the host's save: the host's hello carries it, and a guest writes it to `saves/netplay` and
  plays from there, its own save untouched.

Every copy saves the party as it plays: at the start of each scene, the game as it is then goes to `netplay/party.snap`
(a snapshot, below), the same game in each copy, with the party's id and the hash of its state. A party that ends
early (a crash, a player gone) can go on from there: the host picks **Resume Party** (`MP_NETPLAY_RESUME=1`), the
same players join, in any order, each given the player they were, and once every copy has said it holds the same save (the id, frame and hash the
host offers), each takes it up at the same frame of the new session and the party goes on. A copy without it says so
and the session ends. `tools/netplay_session_test.sh`'s `NETPLAY_TEST_RESUME` tests
it.

Each plays with their own first controller. Only the host's UDP port (7845 by default) must be reachable from the
others: over the internet, a shared VPN or the port forwarded on the host's router.

Each copy keeps a record of its session in `netplay/sessions/<time>-p<player>/`, beside the saves folder (the last
ten): `inputs.mpnp` and its `.hash`, a recording `MP_NETPLAY_REPLAY` plays with `host.save` as the save, and
`session.log`, what the session said, with how long frames waited for the others every 10 seconds. If two copies ever
differ, both name the frame and swap its hashes of each 4 KB page of RDRAM, so each `session.log` names the pages that
differ; both then play on to the same frame, 60 after it, write RDRAM there to `desync-frame<n>.rdram` in the record,
and stop. `tools/netplay_rdram_diff.py` names what differs between the two copies' files by symbol, and replaying each
copy's record on one computer shows which went its own way. `MP_NETPLAY_TEST_POKE=<frame>:<address>` changes a word of
RDRAM at that frame, a desync to test this with (`NETPLAY_TEST_GUEST_ENV` passes it to the session test's guests). During a session, a recording or a replay the patches that work out game state from the window's shape (a board's camera, a widened stage) take it as 16:9 whatever the window (`mp_play_aspect_ratio`, src/game/recomp_api.cpp), so that copies with windows of different shapes keep the same game; `NETPLAY_TEST_GUEST_WINDOW` gives the session test's guests another window. A guest that leaves during a session (its window closed, which sends a goodbye, or quiet for 5
seconds) doesn't end it: the host plays on with that player's inputs empty from the first frame it lacks, and each
frame's `left` mask (carried in the frames the host sends and in recordings) has the game make that player a computer
player (`mario_party.cpp`'s take_over_left). A host that leaves (its goodbye, or quiet for 15 seconds) ends its guests'
sessions, and Resume Party picks the party up again. A session that ends for a reason (copies that differ, the host
gone, a desync, a host not found) shows it in a window before the program closes, as stderr reaches no one from the Windows
build (`MP_NETPLAY_NO_MESSAGES` leaves the window out, for the tests).

Online, the game makes its sound as its frames come: each frame steps the sound driver as many retraces (2 to 4) as
that frame took in real time where it was first played (the host's, or the recorder's), carried with the frame as its
`vis` (`netplay_core.h`'s Frame), so a computer that runs the game slower than 30 frames a second still makes sound as
fast as it plays it, and every copy steps it alike. The scheduler's sound thread starts each of those
steps' sound tasks at once rather than at the next VI as it otherwise would (`mp_netplay_audio_direct`), or the steps
would cost a VI each and the frames run at half speed. `session.log` says every 600 frames how fast they ran, how often
the sound ran dry, and how long a frame spent stepping the sound and in netplay. A stall for a late player holds the sound up, and the frames after it make it up at once. So during a session the sound keeps a cushion of 120 ms, played back to within half a percent faster
or slower to stay there, and after a stall long enough to empty it, it waits until the cushion is full again rather
than crackling (`src/main/main.cpp`'s queue_samples).

`tools/netplay_session_test.sh` plays a session of copies on one computer (`NETPLAY_TEST_PLAYERS`, with packets dropped
at `MP_NETPLAY_DROP` percent to test a lossy link, stalls of up to `MP_NETPLAY_JITTER_MS` before one frame in four,
and `NETPLAY_TEST_GUEST_SAVE` a save the guests start with that the host's replaces).

## The determinism test

On Linux, with the program built (README) and Xvfb, xdotool and a Vulkan driver:

```sh
tools/netplay_replay_test.sh build roms/marioparty.us.z64 out/netplay 5400
```

It records 5400 frames (three minutes at 30 a second) played by keys sent to the window, from a new folder with no
save, then replays them twice, each from a new folder too. A replay that matches prints `every frame matched`; one
that doesn't prints the first frame that differs and the 64 KB chunks of RDRAM that differ, and writes RDRAM there.
Run the recording's other replay with `MP_NETPLAY_DUMP_FRAME=<that frame>` to have RDRAM from both, and compare them
with `tools/netplay_rdram_diff.py`, which names each stretch that differs by its symbol.

A recording starts at power-on and plays from the save the program starts with, so a replay starts from the same
save (the script's folders start with none).

## What the test reaches

The script's keys play the opening, the title and Mushroom Village (overlay 0x69), then go through the warp pipe to the
hub (0x35). Each trip from the hub to the rules' board (0x3E) goes instead, by `MP_NETPLAY_MINIGAMES`, to a minigame's
instructions screen (0x6F) and the minigame, which the four computer players play, as tools/survey.js's `minigame()`
opens one; the hub and its results screen (0x7C) come between them. The script's default list is twelve minigames of
every kind (1 vs 3, 2 vs 2, four players), the three that call `frandom` among them (Ground Pound, Box Mountain
Mayhem, Mario Bandstand), and once they are played the trips go to the rules' board, which the four computers play
(and DK's Jungle Adventure, 0x36, after it). The netplay log names each overlay as the session goes to it
(`[netplay] frame N: overlay 0xNN`) and each minigame it asks for, so a run's log shows what it covered.

The list is the test's, the same for the recording and the replays; a session between players goes where the game
goes.

## Snapshots

A snapshot is the game at a frame, for a copy to take up and play on from: RDRAM and the patches' memory past it
(16 MB), the overlays loaded, the EEPROM, and what of the game the runtime keeps natively (`mario_party.cpp`'s
`extra_state`). It can be taken only where all of the game's state is in them: in the first frames of a scene, before
any of its processes has started (`can_snapshot`), since a process that has run keeps its locals on a native thread's
stack. Each thread's native context stays the taking copy's own. `MP_NETPLAY_SNAPSHOT=<n>` in a replay writes one at
frame n, and `MP_NETPLAY_RESTORE=<file>` in a replay of the same recording takes it up at the first frame it can (the
fourth after power-on) and replays on from there: a 12,000-frame recording taken up at two of its hub's scenes played
on to its end with every frame matching.

Which processes have started is the engine's to say (`mp_prc_started`, lib/mario-party-engine's
`process_threads.cpp`).
