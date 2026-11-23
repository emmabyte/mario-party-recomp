// A session between two copies of the game (MP_NETPLAY_PEER): delay-based lockstep over UDP. Each copy plays one port
// (MP_NETPLAY_PLAYER, 0 or 1) with its own first pad, and each frame's inputs are both players': this copy's pad as it
// sampled it MP_NETPLAY_DELAY frames earlier, and the peer's for the same frame, which the frame waits for. So both
// run the same frames on the same inputs, and each game turns them into the same state (a recording's replay tests
// that it does). The first DELAY frames have no inputs from anyone.
//
// It starts with a handshake, at the first frame: each copy sends the game's identity (netplay_core.h's Game) until it
// has the peer's and knows the peer has its own. Identities that differ end the session on both copies, naming the
// lines that differ: a different ROM, build, mod or option would play differently from the same inputs.
//
// Then the packets, both ways, each frame and again every 16 ms while a frame waits: this copy's inputs from the first
// frame the peer hasn't acknowledged, up to 32 frames of them, its acknowledgement of the peer's (the first frame it
// still needs), and the hashes of the game state it has made for its last frames, which the peer compares with its
// own. A frame whose hashes differ ends the session on both and names the frame: from there the games differ.

#include "netplay_internal.h"
#include "udp.h"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <string>
#include <vector>

using namespace netplay;
using namespace netplay::detail;

namespace {
    constexpr uint32_t SESSION_MAGIC = 0x534E504D; // "MPNS"
    constexpr uint32_t SESSION_VERSION = 2;
    constexpr uint8_t KIND_HELLO = 0;
    constexpr uint8_t KIND_FRAMES = 1;
    constexpr int PLAYERS = 2;
    constexpr int MAX_SEND = 32;
    constexpr int HASHES_SENT = 4;
    constexpr size_t MAX_IDENTITY = 1200;
    using Inputs = std::array<Pad, MAX_SAMPLES>;

    int player = 0;
    int delay = 3;
    int64_t session_frames = -1;
    int timeout_ms = 15000;
    int drop_percent = 0;

    std::string identity;
    bool handshaken = false;
    bool peer_identity_seen = false; // the peer's identity came, and matched
    bool peer_saw_ours = false;      // the peer says it has this copy's

    std::vector<Inputs> local_inputs;   // by frame_no
    std::vector<Inputs> remote_inputs;  // by frame_no
    std::vector<uint8_t> remote_have;   // by frame_no
    uint32_t remote_next = 0;           // the first frame_no of the peer's this copy still needs
    uint32_t peer_ack = 0;              // the first frame_no of this copy's the peer still needs
    std::vector<uint64_t> local_hashes; // by frame_no
    std::deque<std::pair<uint32_t, uint64_t>> remote_hashes; // the peer's, not yet compared
    uint64_t hashes_compared = 0;
    uint32_t compare_from = 0;          // the first frame_no whose hash is yet to be compared

    struct Writer {
        uint8_t buf[1400];
        size_t n = 0;
        void u8(uint8_t v) { if (n < sizeof(buf)) buf[n++] = v; }
        void u16(uint16_t v) { u8(uint8_t(v)); u8(uint8_t(v >> 8)); }
        void u32(uint32_t v) { for (int i = 0; i < 4; i++) u8(uint8_t(v >> (8 * i))); }
        void u64(uint64_t v) { u32(uint32_t(v)); u32(uint32_t(v >> 32)); }
    };
    struct Reader {
        const uint8_t* buf;
        size_t n, at = 0;
        bool ok = true;
        uint8_t u8() { if (at >= n) { ok = false; return 0; } return buf[at++]; }
        uint16_t u16() { uint16_t lo = u8(); return uint16_t(lo | (u8() << 8)); }
        uint32_t u32() { uint32_t v = 0; for (int i = 0; i < 4; i++) v |= uint32_t(u8()) << (8 * i); return v; }
        uint64_t u64() { uint64_t lo = u32(); return lo | (uint64_t(u32()) << 32); }
    };

    void put_pad(Writer& w, const Pad& p) {
        w.u16(p.button); w.u8(uint8_t(p.stick_x)); w.u8(uint8_t(p.stick_y)); w.u8(p.err_no);
    }
    Pad get_pad(Reader& r) {
        Pad p;
        p.button = r.u16(); p.stick_x = int8_t(r.u8()); p.stick_y = int8_t(r.u8()); p.err_no = r.u8();
        return p;
    }

    [[noreturn]] void end_session(int code) {
        fflush(stderr);
        udp::close();
        std::_Exit(code);
    }

    void send(const Writer& w) {
        if (drop_percent > 0 && rand() % 100 < drop_percent) {
            return;
        }
        udp::send(w.buf, w.n);
    }

    void header(Writer& w, uint8_t kind) {
        w.u32(SESSION_MAGIC);
        w.u32(SESSION_VERSION);
        w.u8(kind);
        w.u8(uint8_t(player));
    }

    void send_hello() {
        Writer w;
        header(w, KIND_HELLO);
        w.u8(peer_identity_seen ? 1 : 0);
        size_t n = std::min(identity.size(), MAX_IDENTITY);
        w.u16(uint16_t(n));
        for (size_t i = 0; i < n; i++) {
            w.u8(uint8_t(identity[i]));
        }
        send(w);
    }

    void send_frames() {
        Writer w;
        header(w, KIND_FRAMES);
        w.u32(remote_next);
        uint32_t first = std::max<uint32_t>(peer_ack, uint32_t(delay));
        uint32_t end = uint32_t(local_inputs.size());
        uint32_t count = end > first ? std::min<uint32_t>(end - first, MAX_SEND) : 0;
        w.u32(first);
        w.u8(uint8_t(count));
        for (uint32_t f = first; f < first + count; f++) {
            for (int i = 0; i < game.samples_per_frame; i++) {
                put_pad(w, local_inputs[f][i]);
            }
        }
        uint32_t hashes = std::min<uint32_t>(uint32_t(local_hashes.size()), HASHES_SENT);
        w.u8(uint8_t(hashes));
        for (uint32_t i = 0; i < hashes; i++) {
            uint32_t f = uint32_t(local_hashes.size()) - hashes + i;
            w.u32(f);
            w.u64(local_hashes[f]);
        }
        send(w);
    }

    // "name: value" lines by name.
    std::map<std::string, std::string> lines(const std::string& text) {
        std::map<std::string, std::string> out;
        size_t at = 0;
        while (at < text.size()) {
            size_t end = text.find('\n', at);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(at, end - at);
            size_t colon = line.find(':');
            out[colon == std::string::npos ? line : line.substr(0, colon)] =
                colon == std::string::npos ? "" : line.substr(colon + 1);
            at = end + 1;
        }
        return out;
    }

    void check_identity(const std::string& theirs) {
        std::string ours = identity.substr(0, MAX_IDENTITY);
        if (theirs == ours) {
            peer_identity_seen = true;
            return;
        }
        fprintf(stderr, "[netplay] the peer's copy differs from this one, so the two would play differently:\n");
        auto a = lines(ours), b = lines(theirs);
        for (auto& [name, value] : a) {
            auto it = b.find(name);
            if (it == b.end()) {
                fprintf(stderr, "[netplay]   %s: here%s, the peer has none\n", name.c_str(), value.c_str());
            } else if (it->second != value) {
                fprintf(stderr, "[netplay]   %s: here%s, the peer's%s\n", name.c_str(), value.c_str(), it->second.c_str());
            }
        }
        for (auto& [name, value] : b) {
            if (a.find(name) == a.end()) {
                fprintf(stderr, "[netplay]   %s: none here, the peer's%s\n", name.c_str(), value.c_str());
            }
        }
        for (int i = 0; i < 10; i++) {
            send_hello(); // so the peer can say what differs too
        }
        end_session(4);
    }

    void compare_hashes() {
        while (!remote_hashes.empty() && remote_hashes.front().first < local_hashes.size()) {
            auto [f, h] = remote_hashes.front();
            remote_hashes.pop_front();
            if (local_hashes[f] != h) {
                fprintf(stderr, "[netplay] frame %u differs from the peer's: the session has desynced\n", f);
                for (int i = 0; i < 20; i++) {
                    send_frames(); // so the peer sees this copy's hash for the frame_no too
                }
                end_session(2);
            }
            hashes_compared++;
            compare_from = f + 1;
        }
    }

    void take_frames(Reader& r) {
        uint32_t ack = r.u32();
        uint32_t first = r.u32();
        uint32_t count = r.u8();
        if (!r.ok) {
            return;
        }
        peer_ack = std::max(peer_ack, ack);
        for (uint32_t f = first; f < first + count; f++) {
            Inputs in{};
            for (int i = 0; i < game.samples_per_frame; i++) {
                in[i] = get_pad(r);
            }
            if (!r.ok) {
                return;
            }
            if (remote_inputs.size() <= f) {
                remote_inputs.resize(f + 1);
                remote_have.resize(f + 1, 0);
            }
            remote_inputs[f] = in;
            remote_have[f] = 1;
        }
        while (remote_next < remote_have.size() && remote_have[remote_next]) {
            remote_next++;
        }
        uint32_t hashes = r.u8();
        for (uint32_t i = 0; i < hashes && r.ok; i++) {
            uint32_t f = r.u32();
            uint64_t h = r.u64();
            if (r.ok && (remote_hashes.empty() || f > remote_hashes.back().first) && f >= compare_from) {
                remote_hashes.emplace_back(f, h);
            }
        }
        compare_hashes();
    }

    // Takes in one packet from the peer, if one came within wait_ms. Returns whether one did.
    bool receive_packet(int wait_ms) {
        uint8_t buf[1500];
        size_t n = udp::receive(buf, sizeof(buf), wait_ms);
        if (n == 0) {
            return false;
        }
        Reader r{ buf, n };
        if (r.u32() != SESSION_MAGIC || r.u32() != SESSION_VERSION) {
            return false;
        }
        uint8_t kind = r.u8();
        int from = r.u8();
        if (!r.ok || from == player || from >= PLAYERS) {
            return false;
        }
        if (kind == KIND_HELLO) {
            bool saw = r.u8() != 0;
            uint16_t size = r.u16();
            std::string theirs;
            for (uint16_t i = 0; i < size && r.ok; i++) {
                theirs.push_back(char(r.u8()));
            }
            if (r.ok) {
                check_identity(theirs);
                peer_saw_ours = peer_saw_ours || saw;
                if (handshaken && !saw) {
                    send_hello(); // the peer is still waiting for this copy's identity
                }
            }
        } else if (kind == KIND_FRAMES) {
            // The peer has gone on to its frames, so it has this copy's identity, and has matched it.
            if (peer_identity_seen) {
                peer_saw_ours = true;
            }
            take_frames(r);
        }
        return true;
    }

    void handshake() {
        identity = game.identity != nullptr ? game.identity() : std::string();
        fprintf(stderr, "[netplay] waiting for the peer\n");
        auto until = std::chrono::steady_clock::now() + std::chrono::minutes(2);
        auto next_hello = std::chrono::steady_clock::now();
        while (!(peer_identity_seen && peer_saw_ours)) {
            auto now = std::chrono::steady_clock::now();
            if (now >= next_hello) {
                send_hello();
                next_hello = now + std::chrono::milliseconds(50);
            }
            if (now > until) {
                fprintf(stderr, "[netplay] the peer never answered\n");
                end_session(3);
            }
            receive_packet(16);
        }
        // A last few, so the peer knows this copy has its identity even if one is lost.
        for (int i = 0; i < 3; i++) {
            send_hello();
        }
        handshaken = true;
        fprintf(stderr, "[netplay] the peer's copy matches this one: the session starts\n");
    }
}

bool netplay::detail::open_session(const char* peer) {
    if (const char* p = getenv("MP_NETPLAY_PLAYER")) player = atoi(p);
    if (const char* d = getenv("MP_NETPLAY_DELAY")) delay = std::max(1, atoi(d));
    if (const char* f = getenv("MP_NETPLAY_FRAMES")) session_frames = strtoll(f, nullptr, 10);
    if (const char* t = getenv("MP_NETPLAY_TIMEOUT_MS")) timeout_ms = atoi(t);
    if (const char* d = getenv("MP_NETPLAY_DROP")) drop_percent = atoi(d);
    const char* port_text = getenv("MP_NETPLAY_PORT");
    uint16_t port = uint16_t(atoi(port_text != nullptr ? port_text : "7845"));
    if (player < 0 || player >= PLAYERS) {
        fprintf(stderr, "[netplay] MP_NETPLAY_PLAYER is 0 or 1\n");
        return false;
    }
    if (!udp::open(port, peer)) {
        return false;
    }
    local_inputs.assign(size_t(delay), Inputs{});
    remote_inputs.assign(size_t(delay), Inputs{});
    remote_have.assign(size_t(delay), 1);
    remote_next = uint32_t(delay);
    connected_mask = (1u << PLAYERS) - 1;
    fprintf(stderr, "[netplay] session with %s as player %d, %d frames' delay\n", peer, player + 1, delay);
    return true;
}

Frame netplay::detail::session_frame(uint64_t hash) {
    if (!handshaken) {
        handshake();
    }
    if (session_frames >= 0 && int64_t(frame_no) >= session_frames) {
        // Stay until the peer has this copy's inputs for every frame it plays, then end.
        auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (peer_ack < uint32_t(session_frames) && std::chrono::steady_clock::now() < until) {
            send_frames();
            receive_packet(16);
        }
        fprintf(stderr, "[netplay] played %" PRIu64 " frames with the peer: %" PRIu64 " hashes compared, every one matched\n",
                frame_no, hashes_compared);
        end_session(0);
    }
    local_hashes.push_back(hash);

    // This copy's pad for frame + delay.
    Frame taken = take_local_frame();
    Inputs mine{};
    for (int i = 0; i < game.samples_per_frame; i++) {
        mine[i] = taken.samples[i][0];
    }
    local_inputs.push_back(mine);

    send_frames();
    while (receive_packet(0)) {
    }
    auto last_heard = std::chrono::steady_clock::now();
    bool told = false;
    while (remote_next <= frame_no) {
        if (receive_packet(16)) {
            last_heard = std::chrono::steady_clock::now();
            continue;
        }
        send_frames();
        auto quiet = std::chrono::steady_clock::now() - last_heard;
        if (!told && quiet > std::chrono::seconds(1)) {
            fprintf(stderr, "[netplay] frame %" PRIu64 ": waiting for the peer\n", frame_no);
            told = true;
        }
        if (quiet > std::chrono::milliseconds(timeout_ms)) {
            fprintf(stderr, "[netplay] frame %" PRIu64 ": the peer stopped answering\n", frame_no);
            end_session(3);
        }
    }

    Frame fr;
    fr.connected = uint8_t(connected_mask);
    for (int i = 0; i < game.samples_per_frame; i++) {
        fr.samples[i][player] = local_inputs[frame_no][i];
        fr.samples[i][1 - player] = remote_inputs[frame_no][i];
    }
    return fr;
}
