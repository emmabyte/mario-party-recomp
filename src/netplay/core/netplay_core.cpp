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
//   MP_NETPLAY_DROP (a share of packets to drop, in percent, to test a lossy link).
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
#include <mutex>

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
    constexpr uint32_t VERSION = 2;

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
        for (size_t c = 0; c < CHUNKS; c++) {
            const uint8_t* chunk = rdram + c * CHUNK_SIZE;
            const uint8_t* in = mask.data() + c * CHUNK_SIZE / 4;
            uint64_t h = 0xCBF29CE484222325ULL ^ c;
            for (size_t i = 0; i < CHUNK_SIZE / 4; i++) {
                uint32_t w;
                memcpy(&w, chunk + i * 4, sizeof(w));
                h = mix(h, in[i] ? w : 0);
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

Frame netplay::advance(uint8_t* rdram) {
    FrameHash h = hash_state(rdram);
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
