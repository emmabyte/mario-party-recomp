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
        // The VIs the frame takes, 2 to 4: as many as a frame took in real time on the computer that played it (in a
        // session, the host's; in a recording, the recorder's), for a game whose clocks and sound go by VIs to keep
        // real time when its frames take longer than two (Game's frame_vis).
        uint8_t vis = 2;
        // The ports whose player has left the session (bit n: port n), whose inputs are empty from then on: the game
        // has a computer player take their place.
        uint8_t left = 0;
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
        // Where a session keeps its record (its inputs, hashes, the host's save and its log), for a desync to be
        // replayed and traced later: a folder of the player's. Optional.
        std::string (*data_folder)() = nullptr;
        // A snapshot of the game, for a copy to take up a session where another is (a rejoin, a resync, a saved
        // session): RDRAM, and what the game keeps outside it that its play depends on (extra_state, which
        // set_extra_state puts back). can_snapshot says whether the game is at a frame where RDRAM and that are all
        // its state: nothing on a native stack that another copy's wouldn't have the same. Optional.
        // The VIs that passed in real time since the last call (once a frame), for Frame's vis. Optional: 2 without it.
        int (*frame_vis)() = nullptr;
        bool (*can_snapshot)(const uint8_t* rdram) = nullptr;
        std::vector<uint8_t> (*extra_state)() = nullptr;
        void (*set_extra_state)(const std::vector<uint8_t>& state) = nullptr;
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

    // An online session to start (start): host, for so many players, or join the host at peer ("host" or
    // "host:port", 7845 if none), as the player the host gives this copy unless player (1 to 3) says. port: the UDP
    // port to listen on, the host's 7845 by default, any for a guest.
    struct SessionSettings {
        bool host = false;
        int players = 2;
        std::string peer;
        int player = 0;
        uint16_t port = 0;
        std::string name; // the player's, for the lobby (at most 16 bytes); empty: "Player N"
        bool resume = false; // the host's: resume the last party (party_save) rather than start a new one
    };
    // As the program closes during a session: tells the other copies this one is leaving (the host's guests, that
    // the session is over; the host, that a computer player takes this copy's player's place).
    void leave();
    // A line for the session's log (session.log) and stderr, from the game's adapter.
    void log_line(const char* line);
    // The last party this copy played in, which a host can resume (SessionSettings' resume) if every player's copy has
    // it: false without one. Its frame is the game's, at the start of the scene it was saved at.
    bool party_save(uint64_t& frame, int& players);
    // Sets the session start() starts, unless the environment set one (netplay_core.cpp). Returns false if another
    // mode runs.
    bool set_session(const SessionSettings& settings);

    // A lobby, from a launcher, before the game starts: the session opens and its handshake runs on a thread of its
    // own while the players gather, so the launcher can show who is there. The host's player starts it (lobby_start)
    // with however many have joined; a guest's lobby is ready once its host has started. Then lobby_play makes it the
    // session start() starts, and the launcher starts the game; or lobby_close (Back) leaves it, telling the others.
    // Each player readies up (lobby_set_ready); the host can start once everyone has.
    struct LobbySeat {
        bool present = false;
        bool ready = false;
        int ping_ms = -1;    // the round trip between this player and the host; -1 for the host, or not yet known
        std::string name;
        std::string address; // on the host: where the guest's packets come from
    };
    struct LobbyStatus {
        enum Phase { Waiting, Ready, Failed, Closed };
        Phase phase = Waiting;
        bool host = false;
        int player = -1;                  // this copy's, from 0; -1 until a guest's host has given it one
        std::array<LobbySeat, PORTS> seats;
        std::vector<std::string> share;   // on the host: its addresses (with the port), for the others to join at
        std::string peer;                 // on a guest: the host's address, as typed
        std::string said;                 // the session's lines so far
        std::string reason;               // Failed: the lines that say why
        bool resuming = false;            // the host resumes its last party (a guest knows once the host has said)
    };
    // Returns false if another mode runs, or the session couldn't open (the status says why).
    bool lobby_open(const SessionSettings& settings);
    LobbyStatus lobby_status();
    void lobby_set_ready(bool ready);
    // On the host: starts the session once everyone, the host included, is ready.
    void lobby_start();
    void lobby_close();
    // Once the status is Ready. Returns false before.
    bool lobby_play();

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
