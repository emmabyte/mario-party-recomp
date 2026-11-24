// Netplay's core (netplay_core.h says what it is). Nothing here knows the game: the game says how many samples a frame
// reads, which RDRAM is its state, and what two copies must share.
//
// The environment:
// - MP_NETPLAY_RECORD=<file>: records each frame's inputs to <file> and its hashes to <file>.hash.
// - MP_NETPLAY_RECORD_FRAMES=<n>: ends a recording after n frames, its files complete, and exits.
// - MP_NETPLAY_REPLAY=<file>: plays <file>'s inputs and compares each frame's hash with <file>.hash. It stops at the
//   first frame that differs (with MP_NETPLAY_CONTINUE=1, it goes on and counts them), prints which 64 KB chunks of
//   RDRAM differ, and at the recording's end prints the result and exits, 0 when every frame matched.
// - MP_NETPLAY_DUMP_FRAME=<n>[,<n>...]: writes RDRAM at each frame n to <file>.frame<n>.rdram, to diff two runs.
// - MP_NETPLAY_HOST=<players, 2 to 4>: hosts a session (session.cpp's header says how it goes) on UDP port
//   MP_NETPLAY_PORT (7845 by default), playing port 1.
// - MP_NETPLAY_PEER=<host>[:<port>]: joins the session hosted there, as the player the host gives it, or as
//   MP_NETPLAY_PLAYER (1 to 3: port 2 to 4) of MP_NETPLAY_PLAYERS when both are set, from MP_NETPLAY_PORT (any by
//   default). Without these, the game's Online settings say (src/netplay/mario_party.cpp, set_session).
// - For either: MP_NETPLAY_DELAY (frames of input delay, 3 by default), MP_NETPLAY_FRAMES (ends the session after
//   that many frames, for tests), MP_NETPLAY_TIMEOUT_MS (how long a frame waits for a silent player, 15000) and
//   MP_NETPLAY_DROP (a share of packets to drop, in percent, to test a lossy link). MP_NETPLAY_RESUME=1 on the host
//   resumes the last party (netplay_internal.h's party save) rather than starting a new one.
// - MP_NETPLAY_SNAPSHOT=<n>[,<n>...]: writes a snapshot of the game (Game's can_snapshot) at each frame n to
//   <file>.frame<n>.snap. MP_NETPLAY_RESTORE=<snap>, in a replay: at the first frame the game can take one at (from
//   the fourth), takes up the snapshot's frame in its place and replays on from there, to test that it plays alike.
// - MP_NETPLAY_TEST_POKE=<frame>:<hex address>: changes RDRAM there at that frame, a desync for the tests.
// - MP_NETPLAY_NO_MESSAGES=1: a session that ends for a reason only prints it, with no window for the player to close
//   (for tests; src/netplay/mario_party.cpp).

#include "netplay_core.h"
#include "netplay_internal.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

// The runtime's conversion of a stick's position to the N64's range (N64ModernRuntime's ultramodern/src/input.cpp).
void convert_to_n64_range(float x, float y, int8_t& stick_x, int8_t& stick_y);

namespace netplay::detail {
    Game game{};
    Mode current_mode = Mode::Off;
    uint64_t frame_no = 0;
    std::atomic<uint32_t> connected_mask{ 1 };
}

using namespace netplay;
using namespace netplay::detail;

namespace {
    constexpr uint32_t INPUT_MAGIC = 0x504E504D; // "MPNP"
    constexpr uint32_t HASH_MAGIC = 0x484E504D;  // "MPNH"
    constexpr uint32_t VERSION = 3;

    ultramodern::input::callbacks_t input_callbacks{};
    std::string path;
    FILE* input_file = nullptr;
    FILE* hash_file = nullptr;
    bool keep_going = false;
    std::vector<int64_t> dump_frames;
    int64_t record_frames = -1;

    // A session's record (netplay_internal.h).
    FILE* session_log = nullptr;
    std::string record_folder;

    std::vector<Frame> replay_frames;
    std::vector<FrameHash> replay_hashes;
    uint64_t mismatches = 0;

    // The samples the controller thread takes at the VI, waiting for the next frame.
    std::mutex samples_mutex;
    std::deque<Sample> pending;
    Sample last_sample{};

    uint64_t mix(uint64_t h, uint64_t w) {
        h ^= w;
        h *= 0x9E3779B97F4A7C15ULL;
        return h ^ (h >> 29);
    }

    // The game state's hash: each 64 KB chunk of RDRAM's words, those outside the game's ranges as zeroes, and the
    // chunks' hashes together.
    // page_ring[f % PAGE_FRAMES] holds frame page_frame[...]'s page hashes.
    std::vector<uint64_t> page_ring(PAGE_FRAMES * PAGES);
    std::vector<uint64_t> page_frame(PAGE_FRAMES, UINT64_MAX);
    uint8_t* last_rdram = nullptr;

    FrameHash hash_state(uint8_t* rdram) {
        static std::vector<Range> ranges;
        static std::vector<uint8_t> mask; // one flag a word: whether it is the game's state
        ranges.clear();
        if (game.state_ranges != nullptr) {
            game.state_ranges(rdram, ranges);
        }
        mask.assign(RDRAM_SIZE / 4, 0);
        for (const Range& r : ranges) {
            uint32_t lo = std::max(r.start, 0x80000000u) & ~3u;
            uint32_t hi = std::min<uint64_t>(r.end, 0x80000000ull + RDRAM_SIZE);
            if (lo < hi) {
                memset(mask.data() + (lo - 0x80000000u) / 4, 1, (hi - lo + 3) / 4);
            }
        }
        FrameHash out;
        page_frame[frame_no % PAGE_FRAMES] = frame_no;
        last_rdram = rdram;
        for (size_t c = 0; c < CHUNKS; c++) {
            const uint8_t* chunk = rdram + c * CHUNK_SIZE;
            const uint8_t* in = mask.data() + c * CHUNK_SIZE / 4;
            uint64_t h = 0xCBF29CE484222325ULL ^ c;
            uint64_t* pages = page_ring.data() + (frame_no % PAGE_FRAMES) * PAGES + c * (CHUNK_SIZE / PAGE_SIZE);
            uint64_t page = 0;
            for (size_t i = 0; i < CHUNK_SIZE / 4; i++) {
                uint32_t w;
                memcpy(&w, chunk + i * 4, sizeof(w));
                w = in[i] ? w : 0;
                h = mix(h, w);
                page = mix(page, w);
                if ((i + 1) % (PAGE_SIZE / 4) == 0) {
                    pages[i / (PAGE_SIZE / 4)] = page;
                    page = 0;
                }
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
        for (int i = 0; i < game.samples_per_frame; i++) {
            for (const Pad& p : fr.samples[i]) {
                uint8_t b[5] = { uint8_t(p.button), uint8_t(p.button >> 8), uint8_t(p.stick_x), uint8_t(p.stick_y), p.err_no };
                fwrite(b, 1, 5, f);
            }
        }
        fputc(fr.vis, f);
        fputc(fr.left, f);
    }

    bool read_frame(FILE* f, Frame& fr) {
        int c = fgetc(f);
        if (c == EOF) {
            return false;
        }
        fr.connected = uint8_t(c);
        for (int i = 0; i < game.samples_per_frame; i++) {
            for (Pad& p : fr.samples[i]) {
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
        int vis = fgetc(f);
        if (vis == EOF) {
            return false;
        }
        fr.vis = uint8_t(std::clamp(vis, 2, 4));
        int left = fgetc(f);
        if (left == EOF) {
            return false;
        }
        fr.left = uint8_t(left);
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
        write_u32(input_file, uint32_t(game.samples_per_frame));
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
        if (magic != INPUT_MAGIC || version != VERSION || samples != uint32_t(game.samples_per_frame) || ports != PORTS) {
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
        std::string out = (path.empty() ? std::string("netplay") : path) + ".frame" + std::to_string(frame_no) + ".rdram";
        FILE* f = fopen(out.c_str(), "wb");
        if (f == nullptr) {
            fprintf(stderr, "[netplay] can't write %s\n", out.c_str());
            return;
        }
        fwrite(rdram, 1, RDRAM_SIZE, f);
        fclose(f);
        fprintf(stderr, "[netplay] wrote RDRAM at frame %" PRIu64 " to %s\n", frame_no, out.c_str());
    }

    // The 64 KB chunks of RDRAM whose hashes differ, as address ranges.
    void report_mismatch(const FrameHash& want, const FrameHash& got) {
        fprintf(stderr, "[netplay] frame %" PRIu64 " differs from the recording in:", frame_no);
        for (size_t c = 0; c < CHUNKS; c++) {
            if (want.chunks[c] != got.chunks[c]) {
                fprintf(stderr, " %08X-%08X", uint32_t(0x80000000 + c * CHUNK_SIZE), uint32_t(0x80000000 + (c + 1) * CHUNK_SIZE - 1));
            }
        }
        fprintf(stderr, "\n");
    }

    [[noreturn]] void finish_replay() {
        if (mismatches == 0) {
            fprintf(stderr, "[netplay] replayed %" PRIu64 " frames: every frame matched the recording\n", frame_no);
        } else {
            fprintf(stderr, "[netplay] replayed %" PRIu64 " frames: %" PRIu64 " differed from the recording\n", frame_no, mismatches);
        }
        fflush(stderr);
        std::_Exit(mismatches == 0 ? 0 : 1);
    }

    // This computer's pads now, as the runtime's osContGetReadData reads them.
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
            // writes only the pads that answer).
        }
        return s;
    }
}

// The frame's samples from those taken since the last frame: the latest of them, or the last one again for the ones
// missing.
Frame netplay::detail::take_local_frame() {
    Frame fr;
    fr.connected = uint8_t(connected_mask);
    fr.vis = uint8_t(game.frame_vis != nullptr ? std::clamp(game.frame_vis(), 2, 4) : 2);
    int samples = game.samples_per_frame;
    std::lock_guard lock{ samples_mutex };
    while (int(pending.size()) > samples) {
        pending.pop_front();
    }
    int missing = samples - int(pending.size());
    for (int i = 0; i < samples; i++) {
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

void netplay::init(const Game& g, const ultramodern::input::callbacks_t& callbacks) {
    game = g;
    game.samples_per_frame = std::clamp(game.samples_per_frame, 1, MAX_SAMPLES);
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
    const char* port = getenv("MP_NETPLAY_PORT");
    if (const char* host = getenv("MP_NETPLAY_HOST")) {
        SessionSettings s;
        s.host = true;
        s.players = atoi(host);
        s.resume = getenv("MP_NETPLAY_RESUME") != nullptr;
        s.port = uint16_t(port != nullptr ? atoi(port) : 7845);
        current_mode = open_session(s) ? Mode::Session : Mode::Off;
    } else if (const char* peer = getenv("MP_NETPLAY_PEER")) {
        SessionSettings s;
        s.peer = peer;
        const char* n = getenv("MP_NETPLAY_PLAYERS");
        const char* p = getenv("MP_NETPLAY_PLAYER");
        s.players = n != nullptr ? atoi(n) : 2;
        s.player = p != nullptr ? atoi(p) : 0;
        s.port = uint16_t(port != nullptr ? atoi(port) : 0);
        current_mode = open_session(s) ? Mode::Session : Mode::Off;
    } else if (replay != nullptr) {
        path = replay;
        current_mode = open_replay() ? Mode::Replay : Mode::Off;
    } else if (record != nullptr) {
        path = record;
        current_mode = open_record() ? Mode::Record : Mode::Off;
        if (current_mode == Mode::Record) {
            fprintf(stderr, "[netplay] recording to %s\n", path.c_str());
        }
    }
}

void netplay::detail::open_session_record(int player, const std::vector<uint8_t>& host_save, const std::string& said) {
    if (game.data_folder == nullptr || !path.empty()) {
        return;
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path sessions = fs::path(game.data_folder()) / "sessions";
    fs::create_directories(sessions, ec);
    // Only the last few sessions are kept: a long one's hashes take tens of MB.
    std::vector<fs::path> old;
    for (const auto& entry : fs::directory_iterator(sessions, ec)) {
        if (entry.is_directory()) {
            old.push_back(entry.path());
        }
    }
    std::sort(old.begin(), old.end());
    for (size_t i = 0; i + 9 < old.size(); i++) {
        fs::remove_all(old[i], ec);
    }
    char stamp[32];
    std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
    fs::path folder = sessions / (std::string(stamp) + "-p" + std::to_string(player + 1));
    fs::create_directories(folder, ec);
    record_folder = folder.string();
    path = (folder / "inputs.mpnp").string();
    if (!open_record()) {
        input_file = hash_file = nullptr;
    }
    if (FILE* f = fopen((folder / "host.save").string().c_str(), "wb")) {
        fwrite(host_save.data(), 1, host_save.size(), f);
        fclose(f);
    }
    session_log = fopen((folder / "session.log").string().c_str(), "w");
    if (session_log != nullptr) {
        fputs(said.c_str(), session_log);
        fflush(session_log);
    }
}

void netplay::log_line(const char* line) {
    fputs(line, stderr);
    log_session_line(line);
}

void netplay::detail::log_session_line(const char* line) {
    if (session_log != nullptr) {
        fputs(line, session_log);
        fflush(session_log);
    }
}

void netplay::detail::close_session_record() {
    for (FILE** f : { &input_file, &hash_file, &session_log }) {
        if (*f != nullptr) {
            fclose(*f);
            *f = nullptr;
        }
    }
}

const uint64_t* netplay::detail::page_hashes(uint64_t frame) {
    size_t slot = frame % PAGE_FRAMES;
    return page_frame[slot] == frame ? page_ring.data() + slot * PAGES : nullptr;
}

void netplay::detail::dump_rdram_now(const std::string& name) {
    if (last_rdram == nullptr) {
        return;
    }
    std::string out = (record_folder.empty() ? std::string(".") : record_folder) + "/" + name + ".rdram";
    if (FILE* f = fopen(out.c_str(), "wb")) {
        fwrite(last_rdram, 1, RDRAM_SIZE, f);
        fclose(f);
    }
}

const std::string& netplay::detail::session_record_folder() {
    return record_folder;
}

bool netplay::set_session(const SessionSettings& settings) {
    if (detail::current_mode != Mode::Off) {
        return false;
    }
    detail::current_mode = open_session(settings) ? Mode::Session : Mode::Off;
    return detail::current_mode == Mode::Session;
}

bool netplay::lobby_open(const SessionSettings& settings) {
    if (detail::current_mode != Mode::Off) {
        return false;
    }
    return detail::lobby_begin(settings);
}

void netplay::lobby_close() {
    detail::lobby_end(false);
}

bool netplay::lobby_play() {
    if (detail::current_mode != Mode::Off || lobby_status().phase != LobbyStatus::Ready) {
        return false;
    }
    detail::lobby_end(true);
    detail::current_mode = Mode::Session;
    return true;
}

void netplay::start() {
    if (detail::current_mode == Mode::Session) {
        detail::start_session();
    }
}

Mode netplay::mode() {
    return detail::current_mode;
}

bool netplay::active() {
    return detail::current_mode != Mode::Off;
}

uint64_t netplay::frame() {
    return detail::frame_no;
}

bool netplay::sample_pads() {
    if (detail::current_mode == Mode::Off) {
        return false;
    }
    if (detail::current_mode == Mode::Record || detail::current_mode == Mode::Session) {
        Sample s = sample_local();
        std::lock_guard lock{ samples_mutex };
        pending.push_back(s);
        while (pending.size() > size_t(2 * MAX_SAMPLES)) {
            pending.pop_front();
        }
    }
    return true;
}

// A snapshot (Game's can_snapshot): "MPSS", a version, the frame, the game's extra state, then RDRAM.
namespace {
    constexpr uint32_t SNAP_MAGIC = 0x5353504D; // "MPSS"
    constexpr uint32_t SNAP_VERSION = 3;
    // RDRAM and the 8 MB past it the recomp gives its patches (patches/patches.ld), whose data is the game's too.
    constexpr size_t SNAP_SIZE = 16 * 1024 * 1024;
    std::vector<int64_t> snapshot_frames;
    std::string restore_path;
    bool restored = false;

    // The header: "MPSS", the version, then PartySave's frame, id, hash and players.
    void write_snapshot_header(FILE* f, const PartySave& head, uint32_t extra_size) {
        write_u32(f, SNAP_MAGIC);
        write_u32(f, SNAP_VERSION);
        write_u64(f, head.frame);
        write_u64(f, head.id);
        write_u64(f, head.hash);
        write_u32(f, head.players);
        write_u32(f, head.player);
        write_u32(f, extra_size);
    }

    bool read_snapshot_header(FILE* f, PartySave& head, uint32_t& extra_size) {
        uint32_t magic = 0, version = 0;
        return read_u32(f, magic) && read_u32(f, version) && magic == SNAP_MAGIC && version == SNAP_VERSION &&
            read_u64(f, head.frame) && read_u64(f, head.id) && read_u64(f, head.hash) && read_u32(f, head.players) &&
            read_u32(f, head.player) && read_u32(f, extra_size);
    }

    void write_snapshot(const uint8_t* rdram, uint64_t hash) {
        std::string out = (path.empty() ? std::string("netplay") : path) + ".frame" + std::to_string(frame_no) + ".snap";
        FILE* f = fopen(out.c_str(), "wb");
        if (f == nullptr) {
            fprintf(stderr, "[netplay] can't write %s\n", out.c_str());
            return;
        }
        std::vector<uint8_t> extra = game.extra_state != nullptr ? game.extra_state() : std::vector<uint8_t>{};
        write_snapshot_header(f, PartySave{ 0, frame_no, hash, 0, 0 }, uint32_t(extra.size()));
        fwrite(extra.data(), 1, extra.size(), f);
        fwrite(rdram, 1, SNAP_SIZE, f);
        fclose(f);
        bool safe = game.can_snapshot != nullptr && game.can_snapshot(rdram);
        fprintf(stderr, "[netplay] wrote a snapshot of frame %" PRIu64 " to %s%s\n", frame_no, out.c_str(),
                safe ? "" : " (at a frame the game says isn't safe to)");
    }

    // MP_NETPLAY_RESTORE, in a replay: at the first frame the game can take a snapshot at and take this one up, puts
    // the snapshot's frame in place of this one, and the replay goes on from its frame.
    struct Snapshot {
        uint64_t frame = 0;
        std::vector<uint8_t> extra, ram;
    };
    Snapshot to_restore;

    bool read_snapshot(const std::string& file, Snapshot& out) {
        FILE* f = fopen(file.c_str(), "rb");
        uint32_t extra_size = 0;
        PartySave head;
        if (f == nullptr || !read_snapshot_header(f, head, extra_size)) {
            if (f != nullptr) fclose(f);
            return false;
        }
        out.frame = head.frame;
        out.extra.resize(extra_size);
        out.ram.resize(SNAP_SIZE);
        bool whole = fread(out.extra.data(), 1, extra_size, f) == extra_size && fread(out.ram.data(), 1, SNAP_SIZE, f) == SNAP_SIZE;
        fclose(f);
        return whole;
    }

    void take_up(uint8_t* rdram, const Snapshot& snap) {
        // The runtime keeps a native pointer in each of the game's threads (ultramodern's OSThread: its context, 8 bytes
        // at +0x20), which is this copy's and no other's: it stays as this copy has it, in each thread both copies
        // have (the same priority and id, and a pointer there in both).
        const uint8_t* ram = snap.ram.data();
        auto word = [](const uint8_t* m, size_t at) { uint32_t w; memcpy(&w, m + at, 4); return w; };
        auto host_pointer = [&](const uint8_t* m, size_t at) {
            uint32_t hi = word(m, at + 4);
            return hi >= 0x00005000u && hi < 0x00008000u;
        };
        std::vector<std::pair<size_t, uint64_t>> keep;
        for (size_t at = 0; at + 0x28 <= SNAP_SIZE; at += 8) {
            size_t context = at + 0x20;
            if (host_pointer(rdram, context) && host_pointer(ram, context) && word(rdram, at + 0x14) == word(ram, at + 0x14) &&
                    word(rdram, at + 0x04) == word(ram, at + 0x04) && word(rdram, at + 0x04) < 256) {
                uint64_t v;
                memcpy(&v, rdram + context, 8);
                keep.emplace_back(context, v);
            }
        }
        memcpy(rdram, ram, SNAP_SIZE);
        for (const auto& [at, v] : keep) {
            memcpy(rdram + at, &v, 8);
        }
        if (game.set_extra_state != nullptr) {
            game.set_extra_state(snap.extra);
        }
        fprintf(stderr, "[netplay] frame %" PRIu64 ": took up frame %" PRIu64 " (%zu threads' native contexts kept)\n",
                frame_no, snap.frame, keep.size());
        frame_no = snap.frame;
    }
}

// The party save (netplay_internal.h).
uint64_t netplay::detail::party_id = 0;
int netplay::detail::party_player = 0;

namespace {
    bool was_safe = false;                 // whether the game could be saved at the last frame
    uint64_t party_saved_frame = ~0ull;    // the game frame last saved or taken up, not to save again
    std::atomic<bool> party_writing{ false };
    bool resume_pending = false;
    Snapshot to_resume;

    std::string party_path() {
        if (game.data_folder == nullptr) {
            return {};
        }
        return (std::filesystem::path(game.data_folder()) / "party.snap").string();
    }

    // Writes the game to party.snap on a thread of its own (16 MB takes a while), by way of party.snap.tmp, so that a
    // copy that stops as it writes keeps the save before. One write at a time: a scene that ends sooner is left out.
    void save_party(const uint8_t* rdram, uint64_t hash) {
        std::string out = party_path();
        if (out.empty() || party_writing.exchange(true)) {
            return;
        }
        auto extra = std::make_shared<std::vector<uint8_t>>(game.extra_state != nullptr ? game.extra_state() : std::vector<uint8_t>{});
        auto ram = std::make_shared<std::vector<uint8_t>>(rdram, rdram + SNAP_SIZE);
        uint32_t players = 0;
        for (uint32_t m = connected_mask.load(); m != 0; m &= m - 1) {
            players++;
        }
        PartySave head{ party_id, frame_no, hash, players, uint32_t(party_player) };
        char line[128];
        snprintf(line, sizeof(line), "[netplay] frame %" PRIu64 ": saved the party\n", frame_no);
        log_session_line(line);
        std::thread([out, head, extra, ram] {
            std::string tmp = out + ".tmp";
            bool ok = false;
            if (FILE* f = fopen(tmp.c_str(), "wb")) {
                write_snapshot_header(f, head, uint32_t(extra->size()));
                ok = fwrite(extra->data(), 1, extra->size(), f) == extra->size() &&
                    fwrite(ram->data(), 1, ram->size(), f) == ram->size();
                ok = fclose(f) == 0 && ok;
            }
            std::error_code ec;
            if (ok) {
                std::filesystem::rename(tmp, out, ec);
            }
            if (!ok || ec) {
                fprintf(stderr, "[netplay] couldn't save the party to %s\n", out.c_str());
            }
            party_writing = false;
        }).detach();
    }
}

bool netplay::detail::party_resumable(PartySave& out) {
    std::string file = party_path();
    if (file.empty()) {
        return false;
    }
    FILE* f = fopen(file.c_str(), "rb");
    if (f == nullptr) {
        return false;
    }
    uint32_t extra_size = 0;
    bool ok = read_snapshot_header(f, out, extra_size);
    fclose(f);
    return ok;
}

bool netplay::party_save(uint64_t& frame, int& players) {
    PartySave head;
    if (!detail::party_resumable(head)) {
        return false;
    }
    frame = head.frame;
    players = int(head.players);
    return true;
}

void netplay::detail::resume_party() {
    PartySave head;
    if (party_resumable(head) && read_snapshot(party_path(), to_resume)) {
        party_id = head.id;
        resume_pending = true;
    }
}

Frame netplay::advance(uint8_t* rdram) {
    if (frame_no == 0) {
        if (const char* s = getenv("MP_NETPLAY_SNAPSHOT")) {
            for (const char* p = s; *p != '\0';) {
                char* end;
                snapshot_frames.push_back(strtoll(p, &end, 10));
                p = *end == ',' ? end + 1 : end;
                if (end == p && *p != '\0') {
                    break;
                }
            }
        }
        if (const char* r = getenv("MP_NETPLAY_RESTORE")) {
            restore_path = r;
            if (!read_snapshot(restore_path, to_restore)) {
                fprintf(stderr, "[netplay] %s isn't a snapshot this build reads\n", restore_path.c_str());
                std::_Exit(1);
            }
        }
    }
    if (!restored && !restore_path.empty() && detail::current_mode == Mode::Replay && frame_no >= 4 &&
            game.can_snapshot != nullptr && game.can_snapshot(rdram)) {
        restored = true;
        take_up(rdram, to_restore);
        to_restore = Snapshot{};
    }
    if (resume_pending && detail::current_mode == Mode::Session && frame_no >= 4 && game.can_snapshot != nullptr &&
            game.can_snapshot(rdram)) {
        // A party the players resume: every copy is at the same session frame here, with the same game.
        resume_pending = false;
        take_up(rdram, to_resume);
        to_resume = Snapshot{};
        party_saved_frame = frame_no;
        char line[128];
        snprintf(line, sizeof(line), "[netplay] resumed the party at its frame %" PRIu64 "\n", frame_no);
        log_session_line(line);
    }
    static int64_t poke_frame = -1;
    static uint32_t poke_address = 0;
    if (frame_no == 0) {
        if (const char* poke = getenv("MP_NETPLAY_TEST_POKE")) {
            // <frame>:<address>: changes a word of RDRAM at that frame, a desync for the tests to find.
            poke_frame = strtoll(poke, nullptr, 10);
            if (const char* colon = strchr(poke, ':')) {
                poke_address = uint32_t(strtoul(colon + 1, nullptr, 16)) & ~3u;
            }
        }
    }
    if (int64_t(frame_no) == poke_frame && poke_address >= 0x80000000u && poke_address < 0x80000000u + RDRAM_SIZE) {
        rdram[poke_address - 0x80000000u] ^= 0x5A;
    }
    FrameHash h = hash_state(rdram);
    for (int64_t f : snapshot_frames) {
        if (int64_t(frame_no) == f) {
            write_snapshot(rdram, h.total);
        }
    }
    // The party save, at the start of each scene, once the party has started (not while it waits to resume).
    bool safe = detail::current_mode == Mode::Session && !resume_pending && frame_no >= 4 && game.can_snapshot != nullptr &&
        game.can_snapshot(rdram);
    if (safe && !was_safe && frame_no != party_saved_frame) {
        party_saved_frame = frame_no;
        save_party(rdram, h.total);
    }
    was_safe = safe;
    for (int64_t f : dump_frames) {
        if (int64_t(frame_no) == f) {
            dump_rdram(rdram);
        }
    }

    Frame fr;
    if (detail::current_mode == Mode::Record) {
        fr = take_local_frame();
        write_frame(input_file, fr);
        write_hash(hash_file, h);
        if (frame_no % 60 == 0) {
            fflush(input_file);
            fflush(hash_file);
        }
        if (record_frames >= 0 && int64_t(frame_no) + 1 >= record_frames) {
            fclose(input_file);
            fclose(hash_file);
            fprintf(stderr, "[netplay] recorded %" PRIu64 " frames to %s\n", frame_no + 1, path.c_str());
            fflush(stderr);
            std::_Exit(0);
        }
    } else if (detail::current_mode == Mode::Session) {
        fr = session_frame(h.total);
        if (input_file != nullptr) {
            write_frame(input_file, fr);
            write_hash(hash_file, h);
            if (frame_no % 60 == 0) {
                fflush(input_file);
                fflush(hash_file);
            }
        }
    } else if (detail::current_mode == Mode::Replay) {
        if (frame_no < replay_hashes.size() && replay_hashes[frame_no].total != h.total) {
            report_mismatch(replay_hashes[frame_no], h);
            mismatches++;
            if (!keep_going) {
                dump_rdram(rdram);
                finish_replay();
            }
        }
        if (frame_no >= replay_frames.size()) {
            finish_replay();
        }
        fr = replay_frames[frame_no];
    }
    frame_no++;
    return fr;
}

bool netplay::connected_override(int port, bool* connected) {
    if (detail::current_mode != Mode::Replay && detail::current_mode != Mode::Session) {
        return false;
    }
    *connected = (connected_mask & (1u << port)) != 0;
    return true;
}

void netplay::note_connected(int port, bool connected) {
    if (detail::current_mode == Mode::Replay || detail::current_mode == Mode::Session) {
        return;
    }
    if (connected) {
        connected_mask |= 1u << port;
    } else {
        connected_mask &= ~(1u << port);
    }
}
