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
- Two of the game's threads answer it in real time, and the logic waits on both. A process that asks the SI manager
  (the EEPROM, the Rumble Pak) for something without blocking sleeps a frame at a time until the answer comes: during a
  session it waits for it there and then. The sound driver's thread moves the music's and the sounds' state on as it
  makes sound at each retrace, and the logic reads that back (omMain won't change scenes until the music has stopped,
  objmain copies the music's state into its own flags): during a session the thread gets two retraces from netplay at
  the start of each frame, rather than from the VI handler, makes 544, 528, 528 samples in turn rather than as many as
  the sound card's queue asks for, and the frame starts once it has made them.
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

## What the test hasn't reached yet

The script's keys play the opening, the title and Mushroom Village, so a board's own loading and the minigames are
covered only as far as a recording gets to them. One more of the game's threads is known to answer in real time there:
a board's background loader (threads 0x64 and 0x65, `src/48D90.c`), whose finished pieces the board takes each frame
without waiting (`func_8004A56C`). A replay that differs there names it.
