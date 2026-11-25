// Netplay's Mario Party side: what the game gives netplay's core (src/netplay/core/netplay_core.h), and what makes the
// game's logic turn the same inputs into the same state on every computer. The core is the game's for no game in
// particular; this file is the one that knows the game.
//
// How the game reads its pads: at each VI the controller thread runs func_80013974 (src/engine/pad.c), which reads the
// four pads (osContStartReadData, osContGetReadData) into a ring of 8 entries at D_800D1170, 4 OSContPads (0x18 bytes)
// each, with D_800D12B0 the count waiting, D_800D12B2 the last one read and D_800D12B4 the next to write. Once a frame
// the game's func_80013E84 drains the ring through func_80013B00, one entry a call, into ContBtn, ContBtnTrg,
// ContStkX/Y and the stick history the spinning minigames read (D_800F0A40, D_800F5258, with the count in D_800F5440).
// How many entries wait depends on how the frame lined up with the VIs, so the same play can give the game a frame of
// one entry, or of three, or of none: that is the timing two computers can't share.
//
// While netplay runs (a recording, a replay, a session), then:
// - The hook at func_80013974's start (us.toml) has netplay sample this computer's pads, and skips the function: the
//   game's ring is no longer written at the VI.
// - The hook at func_80013E84's start hands RDRAM to netplay, which hashes the game state and gives back the frame's
//   inputs, and writes exactly SAMPLES entries of them into the game's ring, which the game's own func_80013E84 then
//   drains. A frame here is one call of func_80013E84.
//
// The game's logic also reads clocks that run in real time, which two computers never share, so while netplay runs
// they run by the frame instead (us.toml's hooks):
// - The VI count (D_800F383C), which the VI handler adds one to at each VI, and which the Mini-Game House, the press
//   Start screen and the sound effects' timers read: the VI handler keeps it at two a frame from the frame netplay is
//   on. The main loop and the swap chain, which wait on it for the VIs that really passed, read netplay's count of
//   those instead.
// - The count of frames drawn (D_800F3758), which the main loop adds one to as each frame is drawn: the frame
//   netplay is on, at each frame's start.
// - The CPU's count register, which five copies of Mario Party 4's frandom (Box Mountain Mayhem, Ground Pound, Mario
//   Bandstand, an unused minigame, the staff roll) seed themselves from: a count from the frame netplay is on.
// And it waits on three of the game's threads, whose answers come as real time passes:
// - The SI manager's (src/engine/siman.c): a process that asks it for something and doesn't want to block sleeps a
//   frame at a time until the answer comes. While netplay runs it waits for the answer there and then.
// - The sound driver's (func_800130E8): it makes sound at each retrace and moves the music's and the sounds' state on
//   as it does, and the logic reads that state back (omMain waits for the music to stop before it changes scenes).
//   While netplay runs it gets as many retraces at each frame's start as the frame before took in real time where it
//   was first played (2 to 4, netplay::Frame's vis, from frame_vis), makes a fixed number of samples at each, and the
//   frame waits for it (step_audio).
// - A player who left the session is made a computer player (take_over_left, from netplay::Frame's left).
// - A board's background loader's (src/48D90.c): the board waits for each piece it sends to be finished.
// The game state the hash covers is RDRAM less what `excluded`, the live processes' stacks and the frames' display
// lists name, with the heaps' free blocks zeroed first (state_ranges).
//
// The environment: netplay_core.cpp lists the core's variables. MP_NETPLAY_MINIGAMES is this file's, for the tests.

#include "netplay.h"
#include "recompui/recompui.h"
#include "core/netplay_core.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "SDL.h"
#include "recomp.h"
#include "ultramodern/ultra64.h"
#include "librecomp/mods.hpp"
#include "librecomp/files.hpp"
#include "ultramodern/ultramodern.hpp"
#include "../../lib/mario-party-engine/src/board_options.h"

#ifndef MP_BUILD_COMMIT
#define MP_BUILD_COMMIT "unknown"
#endif

using netplay::Range;

namespace {
    // The ring entries a frame drains: two, as at the game's 30 frames a second a frame spans two of the 60 VIs.
    constexpr int SAMPLES = 2;
    constexpr size_t SAVE_SIZE = 0x200;               // 4 Kbit of EEPROM (main.cpp's SaveType::Eep4k)

    // The game's pad ring (src/engine/pad.c, include/engine/pad.h), by its symbols' addresses (syms/).
    constexpr gpr RING = (int32_t)0x800D1170;   // D_800D1170: 8 entries of 4 OSContPads
    constexpr int RING_ENTRIES = 8;
    constexpr int ENTRY_SIZE = 0x18;
    constexpr int PAD_SIZE = 6;
    constexpr gpr RING_COUNT = (int32_t)0x800D12B0;   // D_800D12B0: entries waiting
    constexpr gpr RING_READ = (int32_t)0x800D12B2;    // D_800D12B2: the last entry read
    constexpr int RING_WRITE_OFFSET = 2;              // D_800D12B4, after D_800D12B2
    constexpr gpr VI_COUNT = (int32_t)0x800F383C;     // D_800F383C: the VIs since boot
    constexpr gpr FRAMES_DRAWN = (int32_t)0x800F3758; // D_800F3758: the frames drawn since boot
    // The CPU's count register runs at 46.875 MHz: 1562500 counts a frame at 30 frames a second.
    constexpr uint64_t COUNTS_PER_FRAME = 1562500;
    constexpr uint32_t RDRAM_SIZE = 8 * 1024 * 1024;

    // The XXH3 of the ROM the runtime checks before it runs (src/main/main.cpp's supported_games).
    constexpr uint64_t ROM_HASH = 0x19f905a0cbc9fe8fULL;
    std::string build_version;

    // The VIs that really passed, and the VI count the game reads while netplay runs.
    std::atomic<uint32_t> real_vis{ 0 };
    std::atomic<uint32_t> game_vis{ 0 };
    // The retraces the sound driver steps for the frame to come: the frame before's vis (netplay::Frame's), the VIs it
    // took in real time where it was played first, so the sound keeps pace with real time when frames run slow.
    int audio_vis = 2;
    // frame_vis: real_vis when it was last asked, and the VIs owed beyond the 4 a frame carries, up to 4 more.
    uint32_t vis_asked = 0;
    int vis_owed = 0;
    // For session.log, every 600 frames: the VIs they took, and how often the sound ran dry meanwhile.
    int vis_window_frames = 0;
    int vis_window_total = 0;
    std::atomic<int> sound_gaps{ 0 };
    double window_audio_ms = 0, window_advance_ms = 0; // the time the frames spent stepping the sound, and in netplay

    // What the game state leaves out: RDRAM the runtime and the game's system threads write as real time passes, which two
    // runs of the same inputs never share and the game's logic never reads back. Found by diffing RDRAM between a
    // recording and its replay (tools/netplay_rdram_diff.py) and named by the decomp's symbols.
    // The sound's, in excluded, which a take-up in the middle of play takes from the save all the same (live_ranges).
    constexpr uint32_t SOUND_DRIVER = 0x800CD9C0;      // the sound driver's data
    constexpr uint32_t AUDIO_HEAP = 0x802E0000;        // the audio heap
    constexpr uint32_t SOUND_THREAD_AREA = 0x800CEB00; // the sound driver's thread's stack, OSThread and queues
    constexpr uint32_t SOUND_THREAD = 0x800D0B30;      // its OSThread, which stays this copy's
    constexpr uint32_t OSTHREAD_SIZE = 0x1B0;
    constexpr Range excluded[] = {
        { 0x800C42B0, 0x800C42B4 }, // D_800C42B0: the main loop's count of frames queued to draw
        { 0x800CD650, 0x800CD9B0 }, // gInitThread, gMainThread: OSThreads, which hold the runtime's own pointers
        // The sound driver's own data (src/B980.c, which nothing else names): its message queue from the game to the
        // audio thread, the players and their state, which the audio thread changes as real time passes. func_8000B198
        // reads it back to wait for a sound to finish: the Options House's sound test, and omMain before a new scene.
        { 0x800CD9C0, 0x800CEB00 },
        { 0x800CEB00, 0x800D10E8 }, // the sound driver's thread: its stack, OSThread (0x800D0B30), queues, task ring
        { 0x800D1D20, 0x800D5ED0 }, // swapChainThread and swapChainStack (src/engine/graphics.c)
        { 0x800D5ED0, 0x800D5FE8 }, // swapChainTask, gSwapChainMesgQueue, gMesgQueue and their messages
        { 0x800D6BA8, 0x800D6D58 }, // the OSThreads of threads 0x65 and 0x64 (src/48D90.c), which hold the runtime's
        { 0x800D7560, 0x800D7710 }, // own pointers
        { 0x800D89D0, 0x800D8CF8 }, // the performance meter's times (osGetTime), queue and display list (src/61B70.c)
        { 0x800E23E8, 0x800E3720 }, // the VI's queue, three threads with their stacks, the scheduler's queues
        { 0x800E3720, 0x800E40D0 }, // the SI manager's thread and stack (src/engine/siman.c)
        { 0x800E42E0, 0x800E4310 }, // the Rumble Paks' state (src/6D4E0.c), which the SI manager's thread counts down
                                    // at each VI and only the motors read
        { 0x800ECB1C, 0x800ECB20 }, // currFrameBuffer
        { 0x800ED0C4, 0x800ED0C8 }, // D_800ED0C4: frames the swap chain has shown
        { 0x800ED398, 0x800ED3E0 }, // the swap chain's task data and queues
        { 0x800ED538, 0x800ED550 }, // a message queue
        { 0x800F0A64, 0x800F2A70 }, // a thread's stack
        { 0x800F3370, 0x800F3388 }, // a message queue
        { 0x800F3758, 0x800F375C }, // D_800F3758: a count of frames shown
        // D_800F37DC: where the frame's display list has got to, which reaches further in a window wider than 4:3
        // (lib/mario-party-engine's camera_view.c adds RT64's commands there) and is only drawn
        { 0x800F37DC, 0x800F37E0 },
        { 0x800F3838, 0x800F3840 }, // nextFrameBuffer, D_800F383C (the VI count)
        { 0x802E0000, 0x80360000 }, // the audio heap (func_8000B00C's, src/B980.c)
    };

    // Left out of the hash but not kept through a resync (live_ranges): D_800ECB2C, a flag of the sound driver's, and
    // ringBufferIndex, where the graphics' messages have got to in gMesgRingBuffer (0x20 of 0x14 bytes,
    // src/engine/graphics.c). Two copies' differ once they have gone back to a save, by what each had in flight; the
    // game's own, from the save, go with the frames it then makes (the copy's in flight kept, it froze).
    constexpr Range unhashed[] = {
        { 0x800ECB2C, 0x800ECB30 },
        { 0x800ECE20, 0x800ED0A0 },
    };

    // The game's heaps (src/59E80.c): a ring of HeapNodes, each 0x10 bytes (size with the node, 0xA5, used, prev, next)
    // before its block.
    constexpr uint32_t MAIN_HEAP = 0x800D6030;   // D_800D6030: HuMemDirectMalloc's
    constexpr uint32_t TEMP_HEAP = 0x800D6040;   // D_800D6040: MallocTemp's
    constexpr uint32_t PROCESS_TOP = 0x800E23CC; // processtop: the first of the scheduler's processes
    constexpr uint32_t HEAP_NODE = 0x10;

    bool in_rdram(uint32_t addr, uint32_t size) {
        return addr >= 0x80000000u && addr - 0x80000000u <= RDRAM_SIZE && size <= RDRAM_SIZE - (addr - 0x80000000u);
    }

    uint32_t read_word(const uint8_t* rdram, uint32_t addr) {
        uint32_t w;
        memcpy(&w, rdram + (addr - 0x80000000u), sizeof(w));
        return w;
    }

    // Zeroes every free block of a heap. A block the game frees keeps whatever was last written to it, and some of that
    // was written as real time passed: the dead part of a process's stack, below where it sleeps, holds what the sound
    // library left there as it read the audio thread's state. A block it allocates again starts with all of it. With
    // the free blocks zeroed at each frame's start, nothing a run leaves behind reaches the next allocation, and the
    // hash covers only what the game keeps.
    void zero_free_blocks(uint8_t* rdram, uint32_t heap_var) {
        uint32_t start = read_word(rdram, heap_var);
        uint32_t node = start;
        for (int n = 0; n < 1 << 16; n++) {
            if (!in_rdram(node, HEAP_NODE)) {
                return;
            }
            uint32_t size = read_word(rdram, node);
            uint32_t flags = read_word(rdram, node + 4);
            if ((flags >> 24) != 0xA5 || size < HEAP_NODE || !in_rdram(node, size)) {
                return;
            }
            if (((flags >> 16) & 0xFF) == 0) {
                memset(rdram + (node + HEAP_NODE - 0x80000000u), 0, size - HEAP_NODE);
            }
            node = read_word(rdram, node + 12);
            if (node == start) {
                return;
            }
        }
    }

    // The stacks of the processes that are alive, which the hash leaves out: as above, what lies below where each
    // sleeps is what was left there, and where that is the scheduler on threads doesn't record (it keeps a process's
    // registers on its thread, not in its jmp_buf). A process's own heap (src/engine/process.c) is its Process, then
    // its stack up to base_sp, 8 bytes past which it ends. What a process keeps in its stack frames shows soon enough
    // in what it does.
    std::vector<std::pair<uint32_t, uint32_t>> process_stacks(const uint8_t* rdram) {
        std::vector<std::pair<uint32_t, uint32_t>> out;
        uint32_t p = read_word(rdram, PROCESS_TOP);
        for (int n = 0; n < 1024 && p != 0 && in_rdram(p, 0x90); n++) {
            uint32_t heap = read_word(rdram, p + 0x18);
            uint32_t base_sp = read_word(rdram, p + 0x28);
            if (in_rdram(heap, HEAP_NODE)) {
                uint32_t lo = heap + read_word(rdram, heap) + HEAP_NODE;
                uint32_t hi = base_sp + 8;
                if (lo < hi && hi - lo <= 0x100000 && in_rdram(lo, hi - lo)) {
                    out.emplace_back(lo & ~3u, hi);
                }
            }
            p = read_word(rdram, p);
        }
        return out;
    }

    // Writes the frame's samples into the game's ring as the next SAMPLES entries after the last one read.
    void fill_ring(uint8_t* rdram, const netplay::Frame& fr) {
        int read = MEM_H(0, RING_READ);
        for (int i = 0; i < SAMPLES; i++) {
            gpr entry = RING + ((read + 1 + i) % RING_ENTRIES) * ENTRY_SIZE;
            for (int port = 0; port < netplay::PORTS; port++) {
                const netplay::Pad& p = fr.samples[i][port];
                MEM_HU(port * PAD_SIZE + 0, entry) = p.button;
                MEM_B(port * PAD_SIZE + 2, entry) = p.stick_x;
                MEM_B(port * PAD_SIZE + 3, entry) = p.stick_y;
                MEM_BU(port * PAD_SIZE + 4, entry) = p.err_no;
            }
        }
        MEM_H(0, RING_COUNT) = SAMPLES;
        MEM_H(RING_WRITE_OFFSET, RING_READ) = (read + 1 + SAMPLES) % RING_ENTRIES;
    }
    // The display lists the logic writes each frame (func_8002B4C0's, src/2C0C0.c: D_800F37DA buffers of D_800F50BA
    // commands, at D_800F3720), which the game only draws and never reads back. A wipe (func_80073560) draws the frame
    // before from the frame buffer the swap chain last chose, currFrameBuffer, which it chose on its own thread as real
    // time passed, and puts its address in the display list.
    std::vector<std::pair<uint32_t, uint32_t>> display_lists(uint8_t* rdram) {
        std::vector<std::pair<uint32_t, uint32_t>> out;
        uint32_t count = (uint8_t)MEM_BU(0, (int32_t)0x800F37DA);
        uint32_t size = 8 * (uint32_t)(uint16_t)MEM_HU(0, (int32_t)0x800F50BA);
        for (uint32_t i = 0; i < count && i < 8; i++) {
            uint32_t dl = (uint32_t)MEM_W(0, (int32_t)(0x800F3720u + 4 * i));
            if (dl >= 0x80000000u && dl + size <= 0x80000000u + RDRAM_SIZE && size != 0) {
                out.push_back({ dl, dl + size });
            }
        }
        return out;
    }

    // The game state, for the hash: RDRAM less what `excluded` names and the stacks of the processes alive, with the
    // heaps' free blocks zeroed first.
    void state_ranges(uint8_t* rdram, std::vector<Range>& out) {
        zero_free_blocks(rdram, MAIN_HEAP);
        zero_free_blocks(rdram, TEMP_HEAP);
        std::vector<Range> left_out(std::begin(excluded), std::end(excluded));
        left_out.insert(left_out.end(), std::begin(unhashed), std::end(unhashed));
        for (auto [lo, hi] : process_stacks(rdram)) {
            left_out.push_back({ lo, hi });
        }
        for (auto [lo, hi] : display_lists(rdram)) {
            left_out.push_back({ lo, hi });
        }
        std::sort(left_out.begin(), left_out.end(), [](const Range& a, const Range& b) { return a.start < b.start; });
        uint32_t at = 0x80000000u;
        for (const Range& r : left_out) {
            if (r.start > at) {
                out.push_back({ at, r.start });
            }
            at = std::max(at, r.end);
        }
        if (at < 0x80000000u + RDRAM_SIZE) {
            out.push_back({ at, 0x80000000u + RDRAM_SIZE });
        }
    }

    // What two copies must share to play alike: the ROM, the build, the mods enabled, and the options that change play
    // (the board's Board Movement and Map Scrolling, lib/mario-party-engine's board_options).
    std::string identity() {
        char rom[32];
        snprintf(rom, sizeof(rom), "%016llX", (unsigned long long)ROM_HASH);
        std::string out = std::string("rom: ") + rom + "\n";
        out += "build: " + build_version + " (" + MP_BUILD_COMMIT + ")\n";
        std::vector<std::string> mods;
        for (const auto& mod : recomp::mods::get_all_mod_details("marioparty")) {
            if (recomp::mods::is_mod_enabled(mod.mod_id)) {
                mods.push_back(mod.mod_id + " " + mod.version.to_string());
            }
        }
        std::sort(mods.begin(), mods.end());
        out += "mods:";
        for (const std::string& m : mods) {
            out += " " + m;
        }
        out += "\n";
        out += std::string("Board Movement: ") + (mp_board_movement_fast() ? "Fast" : "Original") + "\n";
        out += std::string("Map Scrolling: ") + (mp_map_scrolling_fast() ? "Fast" : "Original") + "\n";
        return out;
    }

}

extern "C" gpr mp_survey_vram();

// The test's minigames (MP_NETPLAY_MINIGAMES, "id:g0,g1,g2,g3;..." in hex and decimal): the ones the session plays in
// place of the trips from the hub (0x35) to the rules' board (0x3E), one a trip, each by its instructions screen
// (0x6F) as tools/survey.js's minigame() opens one, so a test reaches the minigames in its 30 minutes, which play
// through the board's turns would reach only after the rules. Each is asked for by the frame's logic alone, on the
// frame the game is on the hub with no request left, so a replay given the same list asks on the same frames.
namespace {
    struct Minigame {
        uint16_t id;
        uint8_t groups[4];
    };
    std::vector<Minigame> minigames;
    size_t next_minigame = 0;
    const Minigame* current_minigame = nullptr;

    constexpr gpr OVERLAY_NOW = (int32_t)0x800F09F4;
    constexpr gpr MINIGAME_ID_WORD = (int32_t)0x800ED5DC; // its low half, GwSystem's minigame ID (0x800ED5DE)
    constexpr gpr GW_PLAYER = (int32_t)0x800F32B0;        // 0x30 bytes a player, the group the top byte
    constexpr int SURVEY_REQUEST = 0x08, SURVEY_FROM = 0x0C, SURVEY_OVERLAY = 0x10, SURVEY_EVENT = 0x14,
                  SURVEY_STAT = 0x18;

    void parse_minigames(const char* p) {
        while (p != nullptr && *p != '\0') {
            char* end;
            Minigame m{};
            m.id = uint16_t(strtoul(p, &end, 0));
            for (int i = 0; i < 4 && *end == (i == 0 ? ':' : ','); i++) {
                m.groups[i] = uint8_t(strtoul(end + 1, &end, 0));
            }
            minigames.push_back(m);
            p = (*end == ';') ? end + 1 : nullptr;
        }
    }

    void step_minigames(uint8_t* rdram) {
        gpr survey = mp_survey_vram();
        if (minigames.empty() || survey == 0 || MEM_W(0, OVERLAY_NOW) != 0x35) {
            return;
        }
        if (MEM_W(SURVEY_REQUEST, survey) == 0) {
            current_minigame = nullptr;
            if (next_minigame < minigames.size()) {
                current_minigame = &minigames[next_minigame++];
                MEM_W(SURVEY_FROM, survey) = 0x3E;
                MEM_W(SURVEY_OVERLAY, survey) = 0x6F;
                MEM_W(SURVEY_EVENT, survey) = 0;
                MEM_W(SURVEY_STAT, survey) = 0x94;
                MEM_W(SURVEY_REQUEST, survey) = 1;
                fprintf(stderr, "[netplay] frame %" PRIu64 ": asking for minigame 0x%02X\n", netplay::frame(),
                        current_minigame->id);
            }
        }
        if (current_minigame != nullptr) {
            for (int i = 0; i < 4; i++) {
                gpr a = GW_PLAYER + i * 0x30;
                MEM_W(0, a) = int32_t((uint32_t(MEM_W(0, a)) & 0x00FFFFFFu) | (uint32_t(current_minigame->groups[i]) << 24));
            }
            MEM_W(0, MINIGAME_ID_WORD) = int32_t((uint32_t(MEM_W(0, MINIGAME_ID_WORD)) & 0xFFFF0000u) | current_minigame->id);
        }
    }
}


// The game's save file: the runtime's, or before the game has started (a lobby's handshake, from the launcher) where
// the runtime will keep it, saves/<game id>.bin in the config folder.
static std::filesystem::path save_file_path() {
    std::filesystem::path path = ultramodern::get_save_file_path();
    if (path.empty()) {
        path = recomp::get_config_path() / "saves" / "marioparty.n64.us.1.0.bin";
    }
    return path;
}

// The save a session plays from: this copy's EEPROM as the runtime read it at the start (its save file, or none).
static std::vector<uint8_t> host_save() {
    std::vector<uint8_t> save(SAVE_SIZE, 0);
    std::ifstream file = recomp::open_input_file_with_backup(save_file_path(), std::ios_base::binary);
    if (file.good()) {
        file.read(reinterpret_cast<char*>(save.data()), std::streamsize(save.size()));
    }
    return save;
}

// A guest plays from the host's save, kept in saves/netplay so that its own save stays as it was: what the session
// saves goes there too.
static void use_host_save(const std::vector<uint8_t>& save) {
    std::filesystem::path path = ultramodern::get_save_file_path();
    std::filesystem::path folder = path.parent_path() / "netplay";
    std::filesystem::create_directories(folder);
    std::vector<uint8_t> whole = save;
    whole.resize(SAVE_SIZE, 0);
    {
        std::ofstream file(folder / path.filename(), std::ios_base::binary | std::ios_base::trunc);
        file.write(reinterpret_cast<const char*>(whole.data()), std::streamsize(whole.size()));
    }
    std::filesystem::remove(folder / (path.filename().u8string() + u8".bak"));
    ultramodern::change_save_file(u8"netplay", path.stem().u8string());
    fprintf(stderr, "[netplay] playing from the host's save\n");
}

void mp_netplay::start() {
    netplay::start();
    // Closing the window during a session says goodbye at once (netplay::leave), so the others needn't wait for this
    // copy to go quiet. The watch runs on the thread that takes the window's events, before the frontend quits.
    static bool watching = false;
    if (!watching && netplay::active()) {
        watching = true;
        SDL_AddEventWatch([](void*, SDL_Event* event) {
            if (event->type == SDL_QUIT ||
                (event->type == SDL_WINDOWEVENT && event->window.event == SDL_WINDOWEVENT_CLOSE)) {
                netplay::leave();
            }
            return 0;
        }, nullptr);
    }
}

void mp_netplay::leave() {
    netplay::leave();
}

void mp_netplay::note_sound_gap() {
    sound_gaps++;
}

bool mp_netplay::active() {
    return netplay::mode() == netplay::Mode::Session;
}

bool mp_netplay::fixed_aspect() {
    return netplay::active();
}

static bool can_snapshot(const uint8_t* rdram);
static void before_take_up(uint8_t* rdram);
static void after_take_up(uint8_t* rdram);
static void give_back(uint8_t* rdram, uint8_t ports);
static void hold_screen(bool waiting);
static std::vector<uint8_t> extra_state();
static void set_extra_state(const std::vector<uint8_t>& in);

void mp_netplay::init(const ultramodern::input::callbacks_t& callbacks, const std::string& version) {
    build_version = version;
    parse_minigames(getenv("MP_NETPLAY_MINIGAMES"));
    netplay::Game game;
    game.samples_per_frame = SAMPLES;
    game.state_ranges = state_ranges;
    game.identity = identity;
    game.data_folder = []() {
        // Beside the saves folder, in the config folder (the program's own folder in portable mode): netplay/. Not
        // from the save's path, which a guest's session moves into saves/netplay.
        static std::string folder = (recomp::get_config_path() / "netplay").string();
        return folder;
    };
    game.can_snapshot = can_snapshot;
    game.before_take_up = before_take_up;
    game.after_take_up = after_take_up;
    game.give_back = give_back;
    game.hold_screen = hold_screen;
    game.live_ranges = [](const uint8_t*, std::vector<netplay::Range>& out) {
        // What the hash leaves out (excluded): the system threads' and the runtime's, as real time passes. But not the
        // sound's: its driver's data, its heap and its thread's stack and queues go with the save, all but the thread's
        // OSThread. The sound steps a frame at a time with the game (step_audio) and its thread waits at the top of its
        // loop between frames, so at a frame's start they are whole; and a scene waits for its sound to stop before it
        // changes (omMain asks func_8000B198), which only the save's own sound does. Kept as they were, the board's music
        // played on after a resync back to a minigame's results, and the game waited for it to stop for good: a black
        // screen. Without the thread's own stack and queues with them, the sound's next task was garbage.
        out.clear();
        for (const netplay::Range& r : excluded) {
            if (r.start == SOUND_THREAD_AREA) {
                out.push_back({ SOUND_THREAD, SOUND_THREAD + OSTHREAD_SIZE });
            } else if (r.start != SOUND_DRIVER && r.start != AUDIO_HEAP) {
                out.push_back(r);
            }
        }
    };
    game.show_resync = [](bool shown) {
        // Over the game, which waits meanwhile: the save on its way, and the game going back to it.
        if (shown) {
            recompui::open_notification("Resyncing", "Getting everyone's game back in step...");
        } else {
            recompui::close_prompt();
        }
    };
    game.frame_vis = []() {
        // The VIs since the last frame, 2 to 4; a longer stall is carried into the next frames, a little at a time.
        uint32_t now = real_vis.load();
        int passed = int(now - vis_asked);
        vis_asked = now;
        int total = std::min(passed + vis_owed, 8);
        int vis = std::clamp(total, 2, 4);
        vis_owed = std::max(0, total - vis);
        vis_window_total += passed;
        if (++vis_window_frames == 600) {
            char line[200];
            snprintf(line, sizeof(line), "[netplay] frame %" PRIu64 ": the last 600 frames ran at %.1f a second; the "
                    "sound ran dry %d times; a frame spent %.1f ms stepping the sound, %.1f ms in netplay\n",
                    netplay::frame(), 600.0 * 60.0 / std::max(vis_window_total, 1), sound_gaps.exchange(0),
                    window_audio_ms / 600, window_advance_ms / 600);
            window_audio_ms = window_advance_ms = 0;
            netplay::log_line(line);
            vis_window_frames = 0;
            vis_window_total = 0;
        }
        return vis;
    };
    game.extra_state = extra_state;
    game.set_extra_state = set_extra_state;
    game.save = host_save;
    game.use_save = use_host_save;
    // A window that waits for the player, but for the tests, where no one would close it (MP_NETPLAY_NO_MESSAGES).
    if (getenv("MP_NETPLAY_NO_MESSAGES") == nullptr) {
        game.show_message = [](const char* title, const char* text) {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, title, text, nullptr);
        };
        // A party that ends goes back to Online Play, the lobby again when the host took everyone there, by starting
        // the program again (restart.cpp); the tests keep ending the program instead.
        game.back_to_menu = [](int, const char* text, const char* lobby) {
            mp_netplay::restart_to_menu(text != nullptr ? text : "", lobby);
        };
        // The host's question when a guest leaves, over the game, which waits meanwhile. asking tells a close of the
        // question (the guest came back) from the host's answer.
        game.ask_left = [](int player, const char* name) {
            static std::atomic<bool> asking{false};
            if (player < 0) {
                if (asking.exchange(false)) {
                    recompui::close_prompt();
                }
                return;
            }
            asking = true;
            std::string header = std::string(name) + " left the party";
            recompui::open_choice_prompt(header,
                "Everyone is waiting. If they join again (Join a Party, with your address), play goes on from the start "
                "of this scene. Or let the computer play for them (they can still join again later), or take "
                "everyone back to the lobby.",
                "Back to lobby", "Computer plays",
                []() { if (asking.exchange(false)) netplay::answer_left(netplay::LeftChoice::Lobby); },
                []() { if (asking.exchange(false)) netplay::answer_left(netplay::LeftChoice::Computer); },
                recompui::ButtonStyle::Primary, recompui::ButtonStyle::Secondary, false);
        };
        game.show_notice = [](const char* text) {
            if (text == nullptr || *text == '\0') {
                recompui::close_prompt();
            } else {
                recompui::open_notification("Waiting for the host", text);
            }
        };
    } else if (const char* choice = getenv("MP_NETPLAY_TEST_LEFT_CHOICE")) {
        // For the tests: the host's answer when a guest leaves (computer or lobby, given at once; wait, none), and the program
        // ends where it would go back to the menu, saying so.
        static bool lobby = strcmp(choice, "lobby") == 0;
        static bool wait = strcmp(choice, "wait") == 0; // no answer: the game waits for the player to join again
        game.ask_left = [](int player, const char*) {
            if (player >= 0 && !wait) {
                netplay::answer_left(lobby ? netplay::LeftChoice::Lobby : netplay::LeftChoice::Computer);
            }
        };
        game.back_to_menu = [](int code, const char* text, const char* to) {
            fprintf(stderr, "[netplay] back to the menu (%d, %s, %s)\n", code, text, to != nullptr ? to : "menu");
            fflush(stderr);
            std::_Exit(0);
        };
    }
    netplay::init(game, callbacks);
}

// The hook at func_80013974's start, on the controller thread at each VI: while netplay runs, samples this computer's
// pads for the next frame and returns 1, which skips the game's read into its ring.
extern "C" int mp_netplay_si_read(uint8_t* rdram) {
    return netplay::sample_pads() ? 1 : 0;
}

// The sound driver's thread (func_800130E8, src/138F0.c) makes the next stretch of sound at each retrace the VI
// handler sends it, and as it does it moves the music's and the sounds' state on: what is playing, fading or done. The
// game's logic reads that state back (omMain's wait for the music and the sounds to stop before it changes scenes,
// func_8000B198; objmain's reading of the music's state into its own flags, func_8006086C), so the frame it sees a
// change on is the frame the thread got to it in real time. During a session the thread gets its retraces from
// netplay instead, two at the start of each frame, and the frame starts once it has made both: the sound is made as
// fast as the game runs, and what the logic reads of it is the same frame for frame on every computer.
//
// One thing the logic reads would come too early so: the music player, which the thread starts a tune on by giving it
// its bank and its sequence and marking it playing (func_8000B844, state 1 in D_800CDAEC), and which takes the bank a
// retrace later, as the library takes events. On a console the next retrace comes as the frame's logic runs, before a
// minigame that reads the tune's channels (Mario Bandstand, func_8000C748 at its start) gets to them; with both of the
// frame's retraces before the logic, it reads a player marked playing with no bank (a null pointer) or the last tune's.
// So a frame whose retraces start a tune has more, up to four, until the player has its bank (one, as the library
// goes), before its logic, and the frames after it have that many fewer: two a frame still, over a few frames.
namespace {
    constexpr uint32_t MUSIC_PLAYER = 0x800CDAD4; // the music's ALSeqPlayer*; its bank is at +0x20
    int audio_borrowed = 0;    // retraces frames have taken early from the frames after them
    uint32_t tune_bank = 0;    // the bank of a tune the thread started in this frame's retraces
    constexpr uint32_t CLIENT_LIST = 0x800E23E0; // the VI handler's clients: next, queue, the messages it wants
    constexpr uint32_t AUDIO_QUEUE = 0x800D0CE0; // D_800D0CE0: the sound driver's thread's queue (func_8001365C's)

    // While the frame waits for the thread, the queue the thread answers on: in the main thread's stack, below where
    // the frame's pad read starts (its hook runs before the function's own frame is made), which nothing uses then.
    // The wait is the runtime's osRecvMesg, so the runtime runs the sound driver's thread meanwhile, as it runs only
    // one of the game's threads at a time.
    std::atomic<uint32_t> audio_reply{ 0 };
    bool audio_answer = false; // the sound driver's thread took a retrace netplay sent, to answer when it is done

    bool audio_registered(const uint8_t* rdram) {
        uint32_t node = read_word(rdram, CLIENT_LIST);
        for (int n = 0; n < 64 && node != 0 && in_rdram(node, 12); n++) {
            if (read_word(rdram, node + 4) == AUDIO_QUEUE) {
                return true;
            }
            node = read_word(rdram, node);
        }
        return false;
    }

    void step_audio(uint8_t* rdram, recomp_context* ctx) {
        if (!audio_registered(rdram)) {
            return;
        }
        uint32_t queue = (uint32_t(ctx->r29) - 0x40) & ~7u;
        osCreateMesgQueue(rdram, (int32_t)queue, (int32_t)(queue + 0x18), 1);
        audio_reply = queue;
        auto retrace = [&]() {
            osSendMesg(rdram, (int32_t)AUDIO_QUEUE, 1, OS_MESG_NOBLOCK);
            osRecvMesg(rdram, (int32_t)queue, NULLPTR, OS_MESG_BLOCK);
        };
        int owed = std::min(audio_borrowed, audio_vis);
        audio_borrowed -= owed;
        for (int i = owed; i < audio_vis; i++) {
            retrace();
        }
        for (int extra = 0; extra < 4 && tune_bank != 0; extra++) {
            uint32_t player = read_word(rdram, MUSIC_PLAYER);
            if (in_rdram(player, 0x24) && read_word(rdram, player + 0x20) == tune_bank) {
                break;
            }
            retrace();
            audio_borrowed++;
        }
        tune_bank = 0;
        audio_reply = 0;
    }
}

// The hook in the sound driver's start of a tune, as it gives the music player its bank.
extern "C" void mp_netplay_tune_bank(gpr bank) {
    if (netplay::active()) {
        tune_bank = uint32_t(bank);
    }
}

// The hook before the scheduler's sound thread waits for a VI (func_80063C38): during a session, it waits for a sound
// task instead (the scheduler's queue of them, D_800ED3A8), so the task runs as soon as the driver sends it, not at the
// next VI: netplay steps the driver several times at a frame's start and each step waits for its task, which would
// otherwise cost a VI each. Returns whether it took one, with the task in the thread's 0x3C($sp) and v0 0, as the
// thread's own NOBLOCK take leaves them.
extern "C" int mp_netplay_audio_direct(uint8_t* rdram, recomp_context* ctx) {
    if (!netplay::active()) {
        return 0;
    }
    osRecvMesg(rdram, (int32_t)0x800ED3A8, (int32_t)(uint32_t(ctx->r29) + 0x3C), OS_MESG_BLOCK);
    ctx->r2 = 0;
    return 1;
}

// The hooks in the sound driver's thread: once it has a message, and as it comes back to wait for the next. During a
// session, a retrace it took is one netplay sent, and it answers once it has made its sound.
extern "C" void mp_netplay_audio_got(uint8_t* rdram, gpr message) {
    audio_answer = netplay::active() && uint32_t(message) == 1;
}

extern "C" void mp_netplay_audio_wait(uint8_t* rdram) {
    if (audio_answer) {
        audio_answer = false;
        uint32_t queue = audio_reply;
        if (queue != 0) {
            osSendMesg(rdram, (int32_t)queue, 0, OS_MESG_NOBLOCK);
        }
    }
}

// The hook after the sound driver's thread asks how much sound is still queued to play (func_80013234), from which it
// works out how many samples to make next: 0x220 less what is queued, plus 0xF0, to 16, plus 16, and no fewer than 0x210
// (D_800D0DC4, D_800D0DBC). A computer's queue empties as its sound card plays it, in real time; during a session the
// answer is one that makes 544, 528, 528 samples in turn by the sound driver's count of retraces (D_800D0DA8), 533 1/3
// a retrace on average, between the driver's fewest and most (32 kHz at 60 retraces a second).
extern "C" gpr mp_netplay_ai_length(uint8_t* rdram, gpr length) {
    if (!netplay::active()) {
        return length;
    }
    uint32_t retraces = read_word(rdram, 0x800D0DA8);
    return retraces % 3 == 0 ? 0x100 * 4 : 0x110 * 4;
}

// A board's background (src/48D90.c) is loaded a piece at a time by two threads of its own: func_8004A890 sends a
// piece to the first (0x65), which reads it from the ROM, the second (0x64) decompresses it into its place, and the
// board takes the finished ones each frame without waiting (func_8004A56C). How many are finished by a frame, and how
// far into RDRAM the one being decompressed has got, is how long those threads ran meanwhile. During a session
// func_8004A890 waits for each piece it sends to be finished: it takes the finished pieces already waiting off the
// queue, waits for its own, and puts them all back in order, so the board finds them as it would have.
namespace {
    constexpr uint32_t LOADER_DONE = 0x800D7FD8; // D_800D7FD8: the finished pieces
    uint32_t loader_sending = 0;
}

// The hooks in func_8004A890: as it sends a piece to the loader, and once it has.
extern "C" void mp_netplay_loader_send(gpr piece) {
    loader_sending = netplay::active() ? uint32_t(piece) : 0;
}

extern "C" void mp_netplay_loader_sent(uint8_t* rdram, recomp_context* ctx) {
    uint32_t piece = loader_sending;
    loader_sending = 0;
    if (piece == 0 || ctx->r2 != 0) {
        return;
    }
    uint32_t slot = (uint32_t(ctx->r29) - 0x10) & ~3u;
    int32_t waiting = MEM_W(8, (int32_t)LOADER_DONE);
    std::vector<int32_t> before;
    for (int32_t i = 0; i < waiting; i++) {
        osRecvMesg(rdram, (int32_t)LOADER_DONE, (int32_t)slot, OS_MESG_NOBLOCK);
        before.push_back(MEM_W(0, (int32_t)slot));
    }
    osRecvMesg(rdram, (int32_t)LOADER_DONE, (int32_t)slot, OS_MESG_BLOCK);
    int32_t done = MEM_W(0, (int32_t)slot);
    for (int32_t m : before) {
        osSendMesg(rdram, (int32_t)LOADER_DONE, m, OS_MESG_NOBLOCK);
    }
    osSendMesg(rdram, (int32_t)LOADER_DONE, done, OS_MESG_NOBLOCK);
    if (uint32_t(done) != piece) {
        fprintf(stderr, "[netplay] the background loader finished %08X while %08X was awaited\n", uint32_t(done), piece);
    }
}

// The hook in the VI handler's send to each of its clients (func_80063AD8): during a session, 1 to leave out the
// retrace to the sound driver's thread, which netplay sends instead.
extern "C" int mp_netplay_vi_client(uint8_t* rdram, gpr queue, gpr message) {
    return netplay::active() && uint32_t(queue) == AUDIO_QUEUE && uint32_t(message) == 1;
}

std::vector<uint32_t> mp_prc_started();
void mp_prc_end(uint32_t key);
std::vector<uint32_t> mp_overlays_loaded();
void mp_overlays_reload(const std::vector<uint32_t>& triples);
void save_read(uint8_t* rdram, PTR(void) rdram_address, uint32_t offset, uint32_t count);
void save_write_ptr(const void* in, uint32_t offset, uint32_t count);
static int prc_survey_left = 0;

// A snapshot of the game (netplay_core.h's Game): safe at a frame where no process has run but the three the game keeps
// for good (gameman.c's, each a loop that sleeps a frame at a time, the same on every copy), as for a few frames as each
// scene starts: every other process's state is all in RDRAM then. What the game keeps outside RDRAM that its play reads:
// the overlays loaded (which code the runtime runs at an address), the EEPROM, and the sound's retraces owed.
static bool is_kept_process(uint32_t func) {
    return func == 0x8001A3DC || func == 0x8001A428 || func == 0x8001A454;
}

static bool can_snapshot(const uint8_t* rdram) {
    std::vector<uint32_t> started = mp_prc_started();
    int alive = 0;
    for (uint32_t p = read_word(rdram, 0x800E23CC); p != 0 && alive < 256; p = read_word(rdram, p)) {
        alive++;
        if (!is_kept_process(read_word(rdram, p + 0x30)) &&
                std::find(started.begin(), started.end(), p) != started.end()) {
            return false;
        }
    }
    return loader_sending == 0;
}

// Before a resync takes up a party save in the middle of play: ends the thread of every process that has started but
// the three the game keeps (whose loops stand where the save's do), so that the save's processes, none of which had
// started, start afresh from RDRAM at their next resume (lib/mario-party-engine's mp_prc_end).
// A board's background loader's two threads (src/48D90.c, patches/background.c's func_8004A41C) live from a board's
// start to its end, natively, each idle in an osRecvMesg on its queue, which lists it as waiting (blocked_on_recv).
// A save has them or not as its scene did, and a copy taking it up has them or not as its own scene does: a resync from
// a minigame's results back to the save made as they began, with the board's loader running again since, lost the
// threads from their queues (the save's list no one waiting) and the board never came back: a black screen for good.
// So a take-up ends this copy's threads first (before_take_up, in the middle of play) and starts them afresh where the
// save has them waiting (after_take_up, every take-up), on the same function and stack as the game, where they wait at
// once on the save's queues.
namespace {
    struct LoaderThread {
        uint32_t thread, id, entry, stack, queue, priority;
    };
    constexpr LoaderThread loader_threads[] = {
        { 0x800D7560, 0x64, 0x8004A394, 0x800D7F20, 0x800D7F20, 1 }, // decompresses (its priority D_800D7710's)
        { 0x800D6BA8, 0x65, 0x8004A31C, 0x800D7560, 0x800D6AF0, 4 }, // reads from the ROM
    };
    constexpr uint32_t LOADER_PRIORITY = 0x800D7710; // D_800D7710: the priority func_8004A530 gives the first

    bool has_context(const uint8_t* rdram, uint32_t thread) {
        uint64_t context;
        memcpy(&context, rdram + (thread - 0x80000000u) + 0x20, sizeof(context));
        return context != 0;
    }
}

static void before_take_up(uint8_t* rdram) {
    for (uint32_t key : mp_prc_started()) {
        if (!is_kept_process(read_word(rdram, key + 0x30))) {
            mp_prc_end(key);
        }
    }
    for (const LoaderThread& t : loader_threads) {
        if (has_context(rdram, t.thread)) {
            osDestroyThread(rdram, (int32_t)t.thread);
        }
    }
}

static void after_take_up(uint8_t* rdram) {
    for (const LoaderThread& t : loader_threads) {
        uint8_t* context = rdram + (t.thread - 0x80000000u) + 0x20;
        if (read_word(rdram, t.queue) != t.thread) {
            // Not waiting in the save: none of this copy's own, nor the save's (another computer's pointer).
            if (has_context(rdram, t.thread)) {
                fprintf(stderr, "[netplay] the save's thread %X wasn't waiting for its loader's queue\n", t.id);
            }
            memset(context, 0, 8);
            continue;
        }
        memset(context, 0, 8);
        memset(rdram + (t.queue - 0x80000000u), 0, 4); // blocked_on_recv: the thread puts itself back
        osCreateThread(rdram, (int32_t)t.thread, (OSId)t.id, (int32_t)t.entry, 0, (int32_t)t.stack, (OSPri)t.priority);
        osStartThread(rdram, (int32_t)t.thread);
        if (t.id == 0x64) {
            osSetThreadPri(rdram, (int32_t)t.thread, (OSPri)read_word(rdram, LOADER_PRIORITY));
        }
        fprintf(stderr, "[netplay] the board's loader thread %X started again, as the save has it\n", t.id);
    }
}

static std::vector<uint8_t> extra_state() {
    std::vector<uint8_t> out;
    auto put = [&](uint32_t v) {
        for (int i = 0; i < 4; i++) out.push_back(uint8_t(v >> (8 * i)));
    };
    std::vector<uint32_t> overlays = mp_overlays_loaded();
    put(uint32_t(overlays.size()));
    for (uint32_t v : overlays) put(v);
    std::vector<uint8_t> eeprom(SAVE_SIZE);
    std::vector<uint8_t> swapped(SAVE_SIZE);
    save_read(swapped.data(), (PTR(void))0x80000000, 0, uint32_t(SAVE_SIZE)); // MEM_B's byte order: i ^ 3
    for (size_t i = 0; i < SAVE_SIZE; i++) eeprom[i] = swapped[i ^ 3];
    put(uint32_t(eeprom.size()));
    out.insert(out.end(), eeprom.begin(), eeprom.end());
    put(uint32_t(audio_borrowed));
    put(uint32_t(audio_vis));
    // The tests' list of minigames (MP_NETPLAY_MINIGAMES): how far it has got.
    put(uint32_t(next_minigame));
    put(current_minigame != nullptr ? uint32_t(current_minigame - minigames.data()) : UINT32_MAX);
    return out;
}

static void set_extra_state(const std::vector<uint8_t>& in) {
    size_t at = 0;
    auto take = [&]() {
        uint32_t v = 0;
        for (int i = 0; i < 4 && at < in.size(); i++) v |= uint32_t(in[at++]) << (8 * i);
        return v;
    };
    std::vector<uint32_t> overlays(take());
    for (uint32_t& v : overlays) v = take();
    mp_overlays_reload(overlays);
    uint32_t eeprom_size = take();
    if (eeprom_size == SAVE_SIZE && at + eeprom_size <= in.size()) {
        save_write_ptr(in.data() + at, 0, eeprom_size);
        at += eeprom_size;
    }
    audio_borrowed = int(take());
    audio_vis = std::clamp(int(take()), 2, 4);
    next_minigame = take();
    uint32_t current = take();
    current_minigame = current < minigames.size() ? &minigames[current] : nullptr;
}

// MP_NETPLAY_PRC_SURVEY: for the first frames of each scene, the processes alive, and which of them have started
// (their state is on a native stack, not only in RDRAM), past the three the game keeps for good (gameman.c's).
static void prc_survey(uint8_t* rdram) {
    static const bool on = getenv("MP_NETPLAY_PRC_SURVEY") != nullptr;
    if (!on || prc_survey_left <= 0) {
        return;
    }
    prc_survey_left--;
    std::vector<uint32_t> started = mp_prc_started();
    std::string line;
    int alive = 0, ran = 0;
    for (uint32_t p = uint32_t(MEM_W(0, (int32_t)0x800E23CC)); p != 0 && alive < 64; p = uint32_t(MEM_W(0, (int32_t)p))) {
        uint32_t func = uint32_t(MEM_W(0x2C + 4, (int32_t)p));
        alive++;
        bool persistent = func == 0x8001A3DC || func == 0x8001A428 || func == 0x8001A454;
        bool has = std::find(started.begin(), started.end(), p) != started.end();
        if (has && !persistent) {
            ran++;
            char item[24];
            snprintf(item, sizeof(item), " %08X", func);
            line += item;
        }
    }
    fprintf(stderr, "[netplay] frame %" PRIu64 ": %d processes, %d started past the 3 kept:%s\n", netplay::frame(),
            alive, ran, line.c_str());
}

// The players (GwPlayer, 0x30 bytes each from 0x800F32B0) whose controller's player has left the session: each is made
// a computer player (flags' bit 0, which the boards and minigames read as it is), every frame so that a new party's
// setup doesn't make them human again. The frame's left mask is the same on every copy, so is this.
// A player who rejoined (netplay::Game's give_back), once the copies have taken up the save they rejoin at: human again.
static void give_back(uint8_t* rdram, uint8_t ports) {
    for (int i = 0; i < 4; i++) {
        gpr player = (int32_t)(0x800F32B0 + 0x30 * i);
        uint8_t port = MEM_BU(3, player);
        if (port < 4 && (ports & (1u << port)) != 0) {
            MEM_HU(6, player) = uint16_t(MEM_HU(6, player) & ~1u);
        }
    }
}

// While a frame waits for the others (netplay::Game's hold_screen): RT64 presents only when the screen changes, so with
// the game waiting it would show nothing new, and the menus drawn at each present (a question for the host, a notice)
// would neither show nor take input. The VI's divot bit, which RT64 doesn't draw by and the game never sets
// (osViSetSpecialFeatures), changes at each call instead, so each VI presents the last picture again; it is set back
// on, as the game's VI mode has it, once the frame comes. Nothing of the game's own state changes.
extern "C" void osViSetSpecialFeatures(uint32_t func);
static void hold_screen(bool waiting) {
    constexpr uint32_t DIVOT_ON = 0x0010, DIVOT_OFF = 0x0020;
    static bool off = false;
    if (waiting) {
        static auto last = std::chrono::steady_clock::time_point{};
        auto now = std::chrono::steady_clock::now();
        if (now - last < std::chrono::milliseconds(15)) {
            return;
        }
        last = now;
        off = !off;
        osViSetSpecialFeatures(off ? DIVOT_OFF : DIVOT_ON);
    } else if (off) {
        off = false;
        osViSetSpecialFeatures(DIVOT_ON);
    }
}

static void take_over_left(uint8_t* rdram, uint8_t left) {
    if (left == 0) {
        return;
    }
    for (int i = 0; i < 4; i++) {
        gpr player = (int32_t)(0x800F32B0 + 0x30 * i);
        uint8_t port = MEM_BU(3, player);
        if (port < 4 && (left & (1u << port)) != 0) {
            MEM_HU(6, player) = uint16_t(MEM_HU(6, player) | 1);
        }
    }
}

// The hook at func_80013E84's start, once a frame on the main thread: while netplay runs, steps the sound driver, hands
// RDRAM to netplay and fills the game's ring with the frame's inputs, which the game's own func_80013E84 then reads.
extern "C" void mp_netplay_pad_frame(uint8_t* rdram, recomp_context* ctx) {
    if (!netplay::active()) {
        return;
    }

    using Ms = std::chrono::duration<double, std::milli>;
    auto audio_start = std::chrono::steady_clock::now();
    step_audio(rdram, ctx);
    window_audio_ms += Ms(std::chrono::steady_clock::now() - audio_start).count();
    prc_survey(rdram);

    // Each scene the session plays, as omOvlGotoEx last went to it (D_800F09F4: a board, a minigame, a menu, by the
    // decomp's overlay numbers), so a log shows what a recording covered.
    static int32_t overlay = -1;
    if (MEM_W(0, (int32_t)0x800F09F4) != overlay) {
        overlay = MEM_W(0, (int32_t)0x800F09F4);
        fprintf(stderr, "[netplay] frame %" PRIu64 ": overlay 0x%02X\n", netplay::frame(), uint32_t(overlay));
        prc_survey_left = 20;
    }

    step_minigames(rdram);

    auto advance_start = std::chrono::steady_clock::now();
    netplay::Frame fr = netplay::advance(rdram);
    window_advance_ms += Ms(std::chrono::steady_clock::now() - advance_start).count();
    fill_ring(rdram, fr);
    audio_vis = std::clamp(int(fr.vis), 2, 4);
    take_over_left(rdram, fr.left);

    // The clocks the frame's logic reads, by the frame.
    game_vis = uint32_t(2 * netplay::frame());
    MEM_W(0, VI_COUNT) = int32_t(game_vis.load());
    MEM_W(0, FRAMES_DRAWN) = int32_t(netplay::frame());
}

// The hook in the VI handler (func_80063B34) between its load of D_800F383C and its store of it plus one: counts the
// VI, and during a session hands back the game's count less one, so that the count the game reads stays the frame's.
extern "C" gpr mp_netplay_vi_tick(uint8_t* rdram, gpr loaded) {
    real_vis++;
    if (!netplay::active()) {
        return loaded;
    }
    return (int32_t)(game_vis.load() - 1);
}

// The hooks after the main loop's and the swap chain's loads of D_800F383C, which wait on it for two VIs: during a
// session, the VIs that really passed.
extern "C" gpr mp_netplay_real_vi(uint8_t* rdram, gpr loaded) {
    if (!netplay::active()) {
        return loaded;
    }
    return (int32_t)real_vis.load();
}

// The hook in RequestSIFunction's wait for the SI manager (src/engine/siman.c), when its first look found no answer:
// during a session, 1, to have it wait for the answer there and then. The game sleeps the process a frame at a time
// until the answer comes, as many frames as the SI manager's thread took in real time, which no two computers share.
extern "C" int mp_netplay_si_wait(void) {
    return netplay::active();
}

// The hooks after the osGetCount call in each copy of frandom: during a session, a count from the frame.
extern "C" gpr mp_netplay_count(uint8_t* rdram, gpr loaded) {
    if (!netplay::active()) {
        return loaded;
    }
    return (int32_t)uint32_t(netplay::frame() * COUNTS_PER_FRAME);
}
