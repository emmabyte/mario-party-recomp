// What netplay_core.cpp and session.cpp share, and nothing outside the core reads.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "netplay_core.h"

namespace netplay::detail {
    // RDRAM as hashed: 8 MB from 0x80000000 in chunks of 64 KB.
    constexpr size_t RDRAM_SIZE = 8 * 1024 * 1024;
    constexpr size_t CHUNK_SIZE = 64 * 1024;
    constexpr size_t CHUNKS = RDRAM_SIZE / CHUNK_SIZE;

    // Finer, for naming what differs when a session desyncs: 4 KB pages, the last PAGE_FRAMES frames' kept.
    constexpr size_t PAGE_SIZE = 4 * 1024;
    constexpr size_t PAGES = RDRAM_SIZE / PAGE_SIZE;
    constexpr size_t PAGE_FRAMES = 256;

    struct FrameHash {
        uint64_t total = 0;
        std::array<uint64_t, CHUNKS> chunks{};
    };

    extern Game game;
    extern Mode current_mode;
    extern uint64_t frame_no; // only the thread that calls advance() reads or writes it
    extern std::atomic<uint32_t> connected_mask;

    Frame take_local_frame();
    // The page hashes of a frame of the last PAGE_FRAMES, or nullptr.
    const uint64_t* page_hashes(uint64_t frame);
    // Writes RDRAM as it is now to the session's record folder (or the working folder), as <name>.rdram.
    void dump_rdram_now(const std::string& name);

    // session.cpp
    // A session that can't start (no such host, a port in use) ends the program, saying why.
    bool open_session(const SessionSettings& settings);
    Frame session_frame(uint64_t hash);
    void start_session();
    // The lobby (netplay_core.h's lobby_open): opens the session and runs its handshake on a thread of its own. Returns
    // false, with the status saying why, if the session couldn't open. lobby_end closes it (play false: Back) or, once
    // it is Ready, ends its thread for the game to play the session.
    bool lobby_begin(const SessionSettings& settings);
    void lobby_end(bool play);

    // netplay_core.cpp: a session's record, in Game's data_folder: sessions/<time>-p<player>/ with inputs.mpnp and its
    // .hash (a recording MP_NETPLAY_REPLAY plays, with host.save as the save), and session.log, the session's lines.
    void open_session_record(int player, const std::vector<uint8_t>& host_save, const std::string& said);
    void log_session_line(const char* line);
    void close_session_record();
    // The folder of the record, empty without one.
    const std::string& session_record_folder();

    // netplay_core.cpp: the party save. During a session every copy writes the game as it is at the start of each scene
    // (Game's can_snapshot) to party.snap in Game's data_folder, the same game in each, with the party's id, which the
    // host chooses as a session starts; a party the players resume keeps its id. party_resumable reads the header of
    // this copy's; resume_party has the copy take it up at the session's first frame the game can (the same in every
    // copy).
    struct PartySave {
        uint64_t id = 0;
        uint64_t frame = 0;
        uint64_t hash = 0;     // the game state's hash at its frame, the same in every copy that saved it
        uint32_t players = 0;
        uint32_t player = 0;   // this copy's player in it, from 0
    };
    extern uint64_t party_id;
    extern int party_player; // this copy's player, for its party saves
    bool party_resumable(PartySave& out);
    void resume_party();

    // The resync (session.cpp): after a desync every copy takes up the host's last party save at the same frame. The
    // save goes about as party.snap's bytes: party_bytes reads this copy's, snapshot_head reads such bytes' header, and
    // resync_with has the copy take them up at the start of its next frame.
    bool party_bytes(std::vector<uint8_t>& out);
    bool snapshot_head(const std::vector<uint8_t>& bytes, PartySave& head);
    void resync_with(std::vector<uint8_t> bytes);
}
