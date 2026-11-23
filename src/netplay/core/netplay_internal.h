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
    bool open_session(const char* peer);
    Frame session_frame(uint64_t hash);
}
