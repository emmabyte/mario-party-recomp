# Netplay

Online play is input lockstep: each player runs the game on their own computer, only the controllers' inputs travel,
and every copy of the game reads the same four controllers on the same frame. That only works if the game turns the
same inputs into the same RDRAM, frame for frame, so the first part here is the test of that: a recording of a
session's inputs and RDRAM hashes, and its replay. `src/netplay/netplay.cpp`'s header says how the game's pad reads
are taken over and what each environment variable does.

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
  runs by the frame instead (us.toml's hooks, `src/netplay/netplay.cpp`'s header):
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
  performance meter, the sound driver's data and heap, the Rumble Paks' state, and the stacks of the processes that are
  alive, below where each sleeps being what was left there (`netplay.cpp`'s `excluded` and `hash_rdram`).
- During a replay the ports with a controller are the recording's (`get_connected_device_info`, `src/main/main.cpp`).

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
