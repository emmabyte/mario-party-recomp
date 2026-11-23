// Netplay's core: lockstep play between copies of a recompiled game, which knows nothing of any one game. A game gives
// it three things (Game, below): its pads' samples once a game frame, the ranges of RDRAM that hold its state, and a
// description of what two copies must share to play alike (the ROM, the build, the mods, the options that change
// play). Netplay gives back each frame's inputs for every port, the same on every copy, and checks that the copies'
// game state stays the same, frame for frame.
//
// Three modes, by the environment (netplay_core.cpp lists the variables):
// - Record: this computer's pads, frame by frame, to a file, with a hash of the game state at each frame.
// - Replay: a recording's inputs, frame by frame, each frame's hash compared with the recording's. A replay that
//   matches is how a game's determinism is tested: the same inputs give the same state.
// - Session: delay-based lockstep with another copy over UDP. Each frame waits for the peer's inputs for it, and
//   the copies compare their hashes as they go.
// With none of them set, nothing runs, and every call returns at once.
//
// What a game must do for the copies to agree is its own: whatever its logic reads that moves with real time (clocks,
// threads that answer as time passes) has to move with the frame instead during a session (mode() != Off).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "ultramodern/input.hpp"

namespace netplay {
    constexpr int PORTS = 4;
    constexpr int MAX_SAMPLES = 4;

    // One port's pad, as libultra's OSContPad holds it.
    struct Pad {
        uint16_t button = 0;
        int8_t stick_x = 0;
        int8_t stick_y = 0;
        uint8_t err_no = 0;
    };
    using Sample = std::array<Pad, PORTS>;

    // A frame's inputs: the ports with a controller, and the samples of every port the frame's logic reads, the
    // game's samples_per_frame of them.
    struct Frame {
        uint8_t connected = 0;
        std::array<Sample, MAX_SAMPLES> samples{};
    };

    // A stretch of RDRAM by its KSEG0 addresses, end excluded.
    struct Range {
        uint32_t start, end;
    };

    struct Game {
        // The samples of the pads a frame's logic reads (a game at 30 frames a second that reads its pads at each of
        // 60 VIs reads two).
        int samples_per_frame = 1;
        // The ranges of RDRAM that hold the game's state this frame, which the hash covers. The game may tidy RDRAM
        // first (zero what it has freed, say), on the frame's thread.
        void (*state_ranges)(uint8_t* rdram, std::vector<Range>& out) = nullptr;
        // Lines of "name: value", everything two copies must share to play alike. A session starts only when both
        // copies' lines match, and names the ones that differ when they don't.
        std::string (*identity)() = nullptr;
        // Shows the player why a session ended (a desync, a player gone, copies that differ) before the program
        // closes, where stderr may reach no one (a windowed program on Windows). Optional.
        void (*show_message)(const char* title, const char* text) = nullptr;
        // The save a session plays from: the host's (save, at most 512 bytes), which each guest plays from in place of
        // its own (use_save, before the game reads it), since the game plays differently from a different save.
        // Optional, for a game with no save.
        std::vector<uint8_t> (*save)() = nullptr;
        void (*use_save)(const std::vector<uint8_t>& host_save) = nullptr;
    };

    enum class Mode { Off, Record, Replay, Session };

    // Reads the mode from the environment and keeps the input callbacks the runtime is given, which netplay samples
    // this computer's pads with.
    void init(const Game& game, const ultramodern::input::callbacks_t& callbacks);
    Mode mode();
    bool active();

    // The frame the game is on: 0 until the first call of advance(), one more after each.
    uint64_t frame();

    // At each VI, on the game's controller thread, in place of its own pad read: takes a sample of this computer's
    // pads for the next frame. Returns whether netplay drives the pads (the game skips its own read if so).
    bool sample_pads();

    // Before the game starts (before it reads its save): a session's handshake, which waits for every player, and the
    // host's save for a guest (Game's use_save). Does nothing but in a session. Without it, the handshake comes at the
    // first frame, and a guest plays from its own save.
    void start();

    // Once a game frame, at the one point the game takes its pads' samples: hashes the game state, records or
    // compares or exchanges it, and returns the frame's inputs, which the game hands its logic in place of its own.
    // During a session this waits for the peer's inputs for the frame. The frame number then goes up by one.
    Frame advance(uint8_t* rdram);

    // Whether a port has a controller, during a replay or a session, when the runtime asks: the recording's or the
    // session's. Returns false when the game's own answer stands; the recorder notes that answer with note_connected.
    bool connected_override(int port, bool* connected);
    void note_connected(int port, bool connected);
}
