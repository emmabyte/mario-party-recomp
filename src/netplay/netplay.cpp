// Netplay's game side (netplay.h). Lockstep netplay needs every copy of the game to turn the same inputs into the same
// RDRAM, frame for frame, so this file does two things: it gives the game its four controllers' inputs at one fixed
// point a frame, and it hashes RDRAM there, so that two runs can be compared. A recording and its replay are the first
// use: record a session, replay it, and the replay's hashes match the recording's frame for frame, or the first frame
// where they don't names the place in RDRAM that differs (docs/netplay.md says how to run one).
//
// How the game reads its pads: at each VI the controller thread runs func_80013974 (src/engine/pad.c), which reads the
// four pads (osContStartReadData, osContGetReadData) into a ring of 8 entries at D_800D1170, 4 OSContPads (0x18 bytes)
// each, with D_800D12B0 the count waiting, D_800D12B2 the last one read and D_800D12B4 the next to write. Once a frame
// the game's func_80013E84 drains the ring through func_80013B00, one entry a call, into ContBtn, ContBtnTrg,
// ContStkX/Y and the stick history the spinning minigames read (D_800F0A40, D_800F5258, with the count in D_800F5440).
// How many entries wait depends on how the frame lined up with the VIs, so the same play can give the game a frame of
// one entry, or of three, or of none: that is the timing a replay can't repeat.
//
// During a session, then:
// - The hook at func_80013974's start (us.toml) samples this computer's pads through the runtime's input callbacks,
//   as osContGetReadData does, into a native queue, and skips the function: the game's ring is no longer written at
//   the VI.
// - The hook at func_80013E84's start hashes RDRAM, takes the frame's inputs (from this computer's samples when
//   recording, from the file when replaying), writes exactly SAMPLES entries of them into the game's ring, and lets
//   the game's own func_80013E84 run on them. So every frame drains SAMPLES entries, of the same inputs, through the
//   game's own code.
// A frame here is one call of func_80013E84.
//
// The game's logic also reads clocks that run in real time, which two computers never share, so during a session
// they run by the frame instead (us.toml's hooks):
// - The VI count (D_800F383C), which the VI handler adds one to at each VI, and which the Mini-Game House, the press
//   Start screen and the sound effects' timers read: the VI handler keeps it at two a frame from the frame netplay is
//   on. The main loop and the swap chain, which wait on it for the VIs that really passed, read netplay's count of
//   those instead.
// - The count of frames drawn (D_800F3758), which the main loop adds one to as each frame is drawn: the frame
//   netplay is on, at each frame's start.
// - The CPU's count register, which five copies of Mario Party 4's frandom (Box Mountain Mayhem, Ground Pound, Mario
//   Bandstand, an unused minigame, the staff roll) seed themselves from: a count from the frame netplay is on.
// And it waits on two of the game's threads, whose answers come as real time passes:
// - The SI manager's (src/engine/siman.c): a process that asks it for something and doesn't want to block sleeps a
//   frame at a time until the answer comes. During a session it waits for the answer there and then.
// - The sound driver's (func_800130E8): it makes sound at each retrace and moves the music's and the sounds' state on
//   as it does, and the logic reads that state back (omMain waits for the music to stop before it changes scenes).
//   During a session it gets two retraces from netplay at each frame's start, makes a fixed number of samples at
//   each, and the frame waits for it (step_audio).
// What the hash leaves out, and the heap's free blocks it zeroes, are by `excluded` and hash_rdram.
//
// The environment:
// - MP_NETPLAY_RECORD=<file>: records each frame's inputs to <file> and its hashes to <file>.hash.
// - MP_NETPLAY_REPLAY=<file>: plays <file>'s inputs and compares each frame's hash with <file>.hash. It stops at the
//   first frame that differs (with MP_NETPLAY_CONTINUE=1, it goes on and counts them), prints which 64 KB chunks of
//   RDRAM differ, and at the recording's end prints the result and exits, 0 when every frame matched.
// - MP_NETPLAY_RECORD_FRAMES=<n>: ends a recording after n frames, its files complete, and exits.
// - MP_NETPLAY_DUMP_FRAME=<n>[,<n>...]: writes RDRAM at each frame n to <file>.frame<n>.rdram, in either mode, to diff two runs.
//
// With neither file set nothing here runs: each hook returns at once and the game plays as it does without netplay.

#include "netplay.h"

#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "recomp.h"
#include "ultramodern/ultra64.h"

// The runtime's conversion of a stick's position to the N64's range (N64ModernRuntime's ultramodern/src/input.cpp).
void convert_to_n64_range(float x, float y, int8_t& stick_x, int8_t& stick_y);

namespace {
    constexpr int PORTS = 4;
    // The ring entries a frame drains: two, as at the game's 30 frames a second a frame spans two of the 60 VIs.
    constexpr int SAMPLES = 2;

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

    // RDRAM as hashed: 8 MB from 0x80000000 in chunks of 64 KB.
    constexpr size_t RDRAM_SIZE = 8 * 1024 * 1024;
    constexpr size_t CHUNK_SIZE = 64 * 1024;
    constexpr size_t CHUNKS = RDRAM_SIZE / CHUNK_SIZE;

    constexpr uint32_t INPUT_MAGIC = 0x504E504D; // "MPNP"
    constexpr uint32_t HASH_MAGIC = 0x484E504D;  // "MPNH"
    constexpr uint32_t VERSION = 1;

    struct Pad {
        uint16_t button = 0;
        int8_t stick_x = 0;
        int8_t stick_y = 0;
        uint8_t err_no = 0;
    };
    using Sample = std::array<Pad, PORTS>;

    struct Frame {
        uint8_t connected = 0;
        std::array<Sample, SAMPLES> samples{};
    };

    struct FrameHash {
        uint64_t total = 0;
        std::array<uint64_t, CHUNKS> chunks{};
    };

    enum class Mode { Off, Record, Replay };

    Mode mode = Mode::Off;
    ultramodern::input::callbacks_t input_callbacks{};
    std::string path;
    FILE* input_file = nullptr;
    FILE* hash_file = nullptr;
    bool keep_going = false;
    std::vector<int64_t> dump_frames;
    int64_t record_frames = -1;

    std::vector<Frame> replay_frames;
    std::vector<FrameHash> replay_hashes;
    uint64_t mismatches = 0;

    std::atomic<uint32_t> connected_mask{ 1 };
    uint64_t frame = 0; // only the main thread, which runs the game's logic, reads it
    // The VIs that really passed, and the VI count the game reads during a session.
    std::atomic<uint32_t> real_vis{ 0 };
    std::atomic<uint32_t> game_vis{ 0 };

    // The local samples the controller thread takes at the VI, waiting for the next frame.
    std::mutex samples_mutex;
    std::deque<Sample> pending;
    Sample last_sample{};

    uint64_t mix(uint64_t h, uint64_t w) {
        h ^= w;
        h *= 0x9E3779B97F4A7C15ULL;
        return h ^ (h >> 29);
    }

    // What the hash leaves out: RDRAM the runtime and the game's system threads write as real time passes, which two
    // runs of the same inputs never share and the game's logic never reads back. Found by diffing RDRAM between a
    // recording and its replay (tools/netplay_rdram_diff.py) and named by the decomp's symbols.
    struct Range {
        uint32_t start, end;
    };
    constexpr Range excluded[] = {
        { 0x800C42B0, 0x800C42B4 }, // D_800C42B0: the main loop's count of frames queued to draw
        { 0x800CD650, 0x800CD9B0 }, // gInitThread, gMainThread: OSThreads, which hold the runtime's own pointers
        // The sound driver's own data (src/B980.c, which nothing else names): its message queue from the game to the
        // audio thread, the players and their state, which the audio thread changes as real time passes. Only the
        // Options House's sound test reads any of it back (func_8000B198), to wait for a sound to finish.
        { 0x800CD9C0, 0x800CEB00 },
        { 0x800CEB00, 0x800D10E8 }, // a thread's stack, the scheduler's thread, its queues and its task ring
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
        { 0x800F3838, 0x800F3840 }, // nextFrameBuffer, D_800F383C (the VI count)
        { 0x802E0000, 0x80360000 }, // the audio heap (func_8000B00C's, src/B980.c)
    };

    // One flag a word of RDRAM: whether the hash leaves it out.
    std::vector<uint8_t> make_mask() {
        std::vector<uint8_t> mask(RDRAM_SIZE / 4, 0);
        for (const Range& r : excluded) {
            for (uint32_t a = r.start; a < r.end; a += 4) {
                mask[(a - 0x80000000) / 4] = 1;
            }
        }
        return mask;
    }
    const std::vector<uint8_t> excluded_mask = make_mask();

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

    FrameHash hash_rdram(uint8_t* rdram) {
        zero_free_blocks(rdram, MAIN_HEAP);
        zero_free_blocks(rdram, TEMP_HEAP);
        static std::vector<uint8_t> frame_mask;
        frame_mask = excluded_mask;
        for (auto [lo, hi] : process_stacks(rdram)) {
            memset(frame_mask.data() + (lo - 0x80000000u) / 4, 1, (hi - lo) / 4);
        }
        FrameHash out;
        for (size_t c = 0; c < CHUNKS; c++) {
            const uint8_t* chunk = rdram + c * CHUNK_SIZE;
            const uint8_t* mask = frame_mask.data() + c * CHUNK_SIZE / 4;
            uint64_t h = 0xCBF29CE484222325ULL ^ c;
            for (size_t i = 0; i < CHUNK_SIZE / 4; i++) {
                uint32_t w;
                memcpy(&w, chunk + i * 4, sizeof(w));
                h = mix(h, mask[i] ? 0 : w);
            }
            out.chunks[c] = h;
            out.total = mix(out.total, h);
        }
        return out;
    }

    void write_u32(FILE* f, uint32_t v) {
        uint8_t b[4] = { uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24) };
        fwrite(b, 1, 4, f);
    }

    void write_u64(FILE* f, uint64_t v) {
        write_u32(f, uint32_t(v));
        write_u32(f, uint32_t(v >> 32));
    }

    bool read_u32(FILE* f, uint32_t& v) {
        uint8_t b[4];
        if (fread(b, 1, 4, f) != 4) {
            return false;
        }
        v = uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
        return true;
    }

    bool read_u64(FILE* f, uint64_t& v) {
        uint32_t lo, hi;
        if (!read_u32(f, lo) || !read_u32(f, hi)) {
            return false;
        }
        v = uint64_t(lo) | (uint64_t(hi) << 32);
        return true;
    }

    void write_frame(FILE* f, const Frame& fr) {
        fputc(fr.connected, f);
        for (const Sample& s : fr.samples) {
            for (const Pad& p : s) {
                uint8_t b[5] = { uint8_t(p.button), uint8_t(p.button >> 8), uint8_t(p.stick_x), uint8_t(p.stick_y), p.err_no };
                fwrite(b, 1, 5, f);
            }
        }
    }

    bool read_frame(FILE* f, Frame& fr) {
        int c = fgetc(f);
        if (c == EOF) {
            return false;
        }
        fr.connected = uint8_t(c);
        for (Sample& s : fr.samples) {
            for (Pad& p : s) {
                uint8_t b[5];
                if (fread(b, 1, 5, f) != 5) {
                    return false;
                }
                p.button = uint16_t(b[0] | (b[1] << 8));
                p.stick_x = int8_t(b[2]);
                p.stick_y = int8_t(b[3]);
                p.err_no = b[4];
            }
        }
        return true;
    }

    void write_hash(FILE* f, const FrameHash& h) {
        write_u64(f, h.total);
        for (uint64_t c : h.chunks) {
            write_u64(f, c);
        }
    }

    bool read_hash(FILE* f, FrameHash& h) {
        if (!read_u64(f, h.total)) {
            return false;
        }
        for (uint64_t& c : h.chunks) {
            if (!read_u64(f, c)) {
                return false;
            }
        }
        return true;
    }

    bool open_record() {
        input_file = fopen(path.c_str(), "wb");
        hash_file = fopen((path + ".hash").c_str(), "wb");
        if (input_file == nullptr || hash_file == nullptr) {
            fprintf(stderr, "[netplay] can't write %s or its .hash\n", path.c_str());
            return false;
        }
        write_u32(input_file, INPUT_MAGIC);
        write_u32(input_file, VERSION);
        write_u32(input_file, SAMPLES);
        write_u32(input_file, PORTS);
        write_u32(hash_file, HASH_MAGIC);
        write_u32(hash_file, VERSION);
        write_u32(hash_file, uint32_t(CHUNKS));
        write_u32(hash_file, uint32_t(CHUNK_SIZE));
        return true;
    }

    bool open_replay() {
        FILE* in = fopen(path.c_str(), "rb");
        FILE* hashes = fopen((path + ".hash").c_str(), "rb");
        if (in == nullptr || hashes == nullptr) {
            fprintf(stderr, "[netplay] can't read %s or its .hash\n", path.c_str());
            return false;
        }
        uint32_t magic = 0, version = 0, samples = 0, ports = 0;
        read_u32(in, magic); read_u32(in, version); read_u32(in, samples); read_u32(in, ports);
        if (magic != INPUT_MAGIC || version != VERSION || samples != SAMPLES || ports != PORTS) {
            fprintf(stderr, "[netplay] %s isn't a recording this build plays\n", path.c_str());
            return false;
        }
        uint32_t chunks = 0, chunk_size = 0;
        read_u32(hashes, magic); read_u32(hashes, version); read_u32(hashes, chunks); read_u32(hashes, chunk_size);
        if (magic != HASH_MAGIC || version != VERSION || chunks != CHUNKS || chunk_size != CHUNK_SIZE) {
            fprintf(stderr, "[netplay] %s.hash isn't a hash file this build reads\n", path.c_str());
            return false;
        }
        Frame fr;
        while (read_frame(in, fr)) {
            replay_frames.push_back(fr);
        }
        FrameHash h;
        while (read_hash(hashes, h)) {
            replay_hashes.push_back(h);
        }
        fclose(in);
        fclose(hashes);
        if (replay_frames.empty()) {
            fprintf(stderr, "[netplay] %s has no frames\n", path.c_str());
            return false;
        }
        connected_mask = replay_frames[0].connected;
        fprintf(stderr, "[netplay] replaying %zu frames from %s\n", replay_frames.size(), path.c_str());
        return true;
    }

    void dump_rdram(const uint8_t* rdram) {
        std::string out = path + ".frame" + std::to_string(frame) + ".rdram";
        FILE* f = fopen(out.c_str(), "wb");
        if (f == nullptr) {
            fprintf(stderr, "[netplay] can't write %s\n", out.c_str());
            return;
        }
        fwrite(rdram, 1, RDRAM_SIZE, f);
        fclose(f);
        fprintf(stderr, "[netplay] wrote RDRAM at frame %" PRIu64 " to %s\n", frame, out.c_str());
    }

    // The 64 KB chunks of RDRAM whose hashes differ, as address ranges.
    void report_mismatch(const FrameHash& want, const FrameHash& got) {
        fprintf(stderr, "[netplay] frame %" PRIu64 " differs from the recording in:", frame);
        for (size_t c = 0; c < CHUNKS; c++) {
            if (want.chunks[c] != got.chunks[c]) {
                fprintf(stderr, " %08X-%08X", uint32_t(0x80000000 + c * CHUNK_SIZE), uint32_t(0x80000000 + (c + 1) * CHUNK_SIZE - 1));
            }
        }
        fprintf(stderr, "\n");
    }

    void finish_replay() {
        if (mismatches == 0) {
            fprintf(stderr, "[netplay] replayed %" PRIu64 " frames: every frame matched the recording\n", frame);
        } else {
            fprintf(stderr, "[netplay] replayed %" PRIu64 " frames: %" PRIu64 " differed from the recording\n", frame, mismatches);
        }
        fflush(stderr);
        std::_Exit(mismatches == 0 ? 0 : 1);
    }

    // This computer's four pads now, as the runtime's osContGetReadData reads them.
    Sample sample_local() {
        Sample s{};
        if (input_callbacks.poll_input != nullptr) {
            input_callbacks.poll_input();
        }
        uint32_t mask = connected_mask;
        for (int port = 0; port < PORTS; port++) {
            uint16_t buttons = 0;
            float x = 0.0f, y = 0.0f;
            if ((mask & (1u << port)) && input_callbacks.get_input != nullptr &&
                    input_callbacks.get_input(port, &buttons, &x, &y)) {
                convert_to_n64_range(x, y, s[port].stick_x, s[port].stick_y);
                s[port].button = buttons;
            }
            // A port without a pad is left as the runtime leaves it, zeroes (librecomp's osContGetReadData_recomp
            // writes only the pads that answer, and the ring starts zeroed).
        }
        return s;
    }

    // The frame's inputs from the samples taken since the last frame: the latest SAMPLES of them, or the last one
    // again for the ones missing.
    Frame take_local_frame() {
        Frame fr;
        fr.connected = uint8_t(connected_mask);
        std::lock_guard lock{ samples_mutex };
        while (pending.size() > SAMPLES) {
            pending.pop_front();
        }
        int missing = SAMPLES - int(pending.size());
        for (int i = 0; i < SAMPLES; i++) {
            if (i < missing) {
                fr.samples[i] = last_sample;
            } else {
                fr.samples[i] = pending.front();
                pending.pop_front();
                last_sample = fr.samples[i];
            }
        }
        return fr;
    }

    // Writes the frame's samples into the game's ring as the next SAMPLES entries after the last one read.
    void fill_ring(uint8_t* rdram, const Frame& fr) {
        int read = MEM_H(0, RING_READ);
        for (int i = 0; i < SAMPLES; i++) {
            gpr entry = RING + ((read + 1 + i) % RING_ENTRIES) * ENTRY_SIZE;
            for (int port = 0; port < PORTS; port++) {
                const Pad& p = fr.samples[i][port];
                MEM_HU(port * PAD_SIZE + 0, entry) = p.button;
                MEM_B(port * PAD_SIZE + 2, entry) = p.stick_x;
                MEM_B(port * PAD_SIZE + 3, entry) = p.stick_y;
                MEM_BU(port * PAD_SIZE + 4, entry) = p.err_no;
            }
        }
        MEM_H(0, RING_COUNT) = SAMPLES;
        MEM_H(RING_WRITE_OFFSET, RING_READ) = (read + 1 + SAMPLES) % RING_ENTRIES;
    }
}

void netplay::init(const ultramodern::input::callbacks_t& callbacks) {
    input_callbacks = callbacks;
    const char* record = getenv("MP_NETPLAY_RECORD");
    const char* replay = getenv("MP_NETPLAY_REPLAY");
    const char* dump = getenv("MP_NETPLAY_DUMP_FRAME");
    keep_going = getenv("MP_NETPLAY_CONTINUE") != nullptr;
    for (const char* p = dump; p != nullptr && *p != '\0';) {
        char* end;
        dump_frames.push_back(strtoll(p, &end, 10));
        p = (*end == ',') ? end + 1 : nullptr;
    }
    if (const char* limit = getenv("MP_NETPLAY_RECORD_FRAMES")) {
        record_frames = strtoll(limit, nullptr, 10);
    }
    if (replay != nullptr) {
        path = replay;
        mode = open_replay() ? Mode::Replay : Mode::Off;
    } else if (record != nullptr) {
        path = record;
        mode = open_record() ? Mode::Record : Mode::Off;
        if (mode == Mode::Record) {
            fprintf(stderr, "[netplay] recording to %s\n", path.c_str());
        }
    }
}

bool netplay::active() {
    return mode != Mode::Off;
}

bool netplay::connected_override(int port, bool* connected) {
    if (mode != Mode::Replay) {
        return false;
    }
    *connected = (connected_mask & (1u << port)) != 0;
    return true;
}

void netplay::note_connected(int port, bool connected) {
    if (mode == Mode::Replay) {
        return;
    }
    if (connected) {
        connected_mask |= 1u << port;
    } else {
        connected_mask &= ~(1u << port);
    }
}

// The hook at func_80013974's start, on the controller thread at each VI: during a session, samples this computer's
// pads for the next frame and returns 1, which skips the game's read into its ring.
extern "C" int mp_netplay_si_read(uint8_t* rdram) {
    if (mode == Mode::Off) {
        return 0;
    }
    if (mode == Mode::Record) {
        Sample s = sample_local();
        std::lock_guard lock{ samples_mutex };
        pending.push_back(s);
        while (pending.size() > RING_ENTRIES) {
            pending.pop_front();
        }
    }
    return 1;
}

// The hook at func_80013E84's start, once a frame on the main thread: during a session, hashes RDRAM and fills the
// game's ring with the frame's inputs, which the game's own func_80013E84 then reads.

// The sound driver's thread (func_800130E8, src/138F0.c) makes the next stretch of sound at each retrace the VI
// handler sends it, and as it does it moves the music's and the sounds' state on: what is playing, fading or done. The
// game's logic reads that state back (omMain's wait for the music and the sounds to stop before it changes scenes,
// func_8000B198; objmain's reading of the music's state into its own flags, func_8006086C), so the frame it sees a
// change on is the frame the thread got to it in real time. During a session the thread gets its retraces from
// netplay instead, two at the start of each frame, and the frame starts once it has made both: the sound is made as
// fast as the game runs, and what the logic reads of it is the same frame for frame on every computer.
namespace {
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
        for (int i = 0; i < 2; i++) {
            osSendMesg(rdram, (int32_t)AUDIO_QUEUE, 1, OS_MESG_NOBLOCK);
            osRecvMesg(rdram, (int32_t)queue, NULLPTR, OS_MESG_BLOCK);
        }
        audio_reply = 0;
    }
}

// The hooks in the sound driver's thread: once it has a message, and as it comes back to wait for the next. During a
// session, a retrace it took is one netplay sent, and it answers once it has made its sound.
extern "C" void mp_netplay_audio_got(uint8_t* rdram, gpr message) {
    audio_answer = mode != Mode::Off && uint32_t(message) == 1;
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
    if (mode == Mode::Off) {
        return length;
    }
    uint32_t retraces = read_word(rdram, 0x800D0DA8);
    return retraces % 3 == 0 ? 0x100 * 4 : 0x110 * 4;
}

// The hook in the VI handler's send to each of its clients (func_80063AD8): during a session, 1 to leave out the
// retrace to the sound driver's thread, which netplay sends instead.
extern "C" int mp_netplay_vi_client(uint8_t* rdram, gpr queue, gpr message) {
    return mode != Mode::Off && uint32_t(queue) == AUDIO_QUEUE && uint32_t(message) == 1;
}

extern "C" void mp_netplay_pad_frame(uint8_t* rdram, recomp_context* ctx) {
    if (mode == Mode::Off) {
        return;
    }

    step_audio(rdram, ctx);

    FrameHash h = hash_rdram(rdram);
    for (int64_t f : dump_frames) {
        if (int64_t(frame) == f) {
            dump_rdram(rdram);
        }
    }

    Frame fr;
    if (mode == Mode::Record) {
        fr = take_local_frame();
        write_frame(input_file, fr);
        write_hash(hash_file, h);
        if (frame % 60 == 0) {
            fflush(input_file);
            fflush(hash_file);
        }
        if (record_frames >= 0 && int64_t(frame) + 1 >= record_frames) {
            fclose(input_file);
            fclose(hash_file);
            fprintf(stderr, "[netplay] recorded %" PRIu64 " frames to %s\n", frame + 1, path.c_str());
            fflush(stderr);
            std::_Exit(0);
        }
    } else {
        if (frame < replay_hashes.size() && replay_hashes[frame].total != h.total) {
            report_mismatch(replay_hashes[frame], h);
            mismatches++;
            if (!keep_going) {
                dump_rdram(rdram);
                finish_replay();
            }
        }
        if (frame >= replay_frames.size()) {
            finish_replay();
        }
        fr = replay_frames[frame];
    }

    fill_ring(rdram, fr);
    frame++;

    // The clocks the frame's logic reads, by the frame.
    game_vis = uint32_t(2 * frame);
    MEM_W(0, VI_COUNT) = int32_t(game_vis.load());
    MEM_W(0, FRAMES_DRAWN) = int32_t(frame);
}

// The hook in the VI handler (func_80063B34) between its load of D_800F383C and its store of it plus one: counts the
// VI, and during a session hands back the game's count less one, so that the count the game reads stays the frame's.
extern "C" gpr mp_netplay_vi_tick(uint8_t* rdram, gpr loaded) {
    real_vis++;
    if (mode == Mode::Off) {
        return loaded;
    }
    return (int32_t)(game_vis.load() - 1);
}

// The hooks after the main loop's and the swap chain's loads of D_800F383C, which wait on it for two VIs: during a
// session, the VIs that really passed.
extern "C" gpr mp_netplay_real_vi(uint8_t* rdram, gpr loaded) {
    if (mode == Mode::Off) {
        return loaded;
    }
    return (int32_t)real_vis.load();
}

// The hook in RequestSIFunction's wait for the SI manager (src/engine/siman.c), when its first look found no answer:
// during a session, 1, to have it wait for the answer there and then. The game sleeps the process a frame at a time
// until the answer comes, as many frames as the SI manager's thread took in real time, which no two computers share.
extern "C" int mp_netplay_si_wait(void) {
    return mode != Mode::Off;
}

// The hooks after the osGetCount call in each copy of frandom: during a session, a count from the frame.
extern "C" gpr mp_netplay_count(uint8_t* rdram, gpr loaded) {
    if (mode == Mode::Off) {
        return loaded;
    }
    return (int32_t)uint32_t(frame * COUNTS_PER_FRAME);
}
