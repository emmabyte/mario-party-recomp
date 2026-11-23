// What netplay_core.cpp and session.cpp share, and nothing outside the core reads.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "netplay_core.h"

namespace netplay::detail {
    // RDRAM as hashed: 8 MB from 0x80000000 in chunks of 64 KB.
    constexpr size_t RDRAM_SIZE = 8 * 1024 * 1024;
    constexpr size_t CHUNK_SIZE = 64 * 1024;
    constexpr size_t CHUNKS = RDRAM_SIZE / CHUNK_SIZE;

    struct FrameHash {
        uint64_t total = 0;
        std::array<uint64_t, CHUNKS> chunks{};
    };

    extern Game game;
    extern Mode current_mode;
    extern uint64_t frame_no; // only the thread that calls advance() reads or writes it
    extern std::atomic<uint32_t> connected_mask;

    Frame take_local_frame();

    // session.cpp
    // A session that can't start (no such host, a port in use) ends the program, saying why.
    bool open_session(const SessionSettings& settings);
    Frame session_frame(uint64_t hash);
    void start_session();

    // netplay_core.cpp: a session's record, in Game's data_folder: sessions/<time>-p<player>/ with inputs.mpnp and its
    // .hash (a recording MP_NETPLAY_REPLAY plays, with host.save as the save), and session.log, the session's lines.
    void open_session_record(int player, const std::vector<uint8_t>& host_save, const std::string& said);
    void log_session_line(const char* line);
    void close_session_record();
    // The folder of the record, empty without one.
    const std::string& session_record_folder();
}
