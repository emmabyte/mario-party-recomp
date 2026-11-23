// A session between copies of the game: delay-based lockstep over UDP for two to four players, through a host. The
// host (MP_NETPLAY_HOST=<players>) plays port 1 and listens on MP_NETPLAY_PORT; each guest (MP_NETPLAY_PEER=<host's
// address>:<port>, MP_NETPLAY_PLAYERS=<players>, MP_NETPLAY_PLAYER=<its port, 1 to 3>) plays its port with its own first pad and sends only to the
// host. A frame's inputs are every player's pad as each copy sampled it MP_NETPLAY_DELAY frames earlier: each guest
// sends the host its own, the host puts each frame together once it has every player's, and sends the whole frame to
// every guest. So every copy runs the same frames on the same inputs, and each game turns them into the same state (a
// recording's replay tests that it does). Only the host needs a port others can reach. The first DELAY frames have no
// inputs from anyone.
//
// It starts with a handshake, at the first frame: each guest sends the host the game's identity (netplay_core.h's
// Game) until the host has it and has answered with its own. The session starts once the host has every guest's.
// Identities that differ end the session, naming the lines that differ: a different ROM, build, mod or option would
// play differently from the same inputs. The host's hello carries its save too, and each guest plays from it in place
// of its own (Game's save and use_save), since a different save plays differently. netplay::start runs the handshake
// before the game starts, so a guest has the host's save before the game reads it.
//
// Then the packets, each frame and again every 16 ms while a frame waits. A guest's to the host: its inputs from the
// first frame the host hasn't acknowledged, the first whole frame it still needs, and the hashes of the game state it
// made for its last frames. The host's to each guest: the whole frames from the first that guest still needs, the
// first of that guest's inputs it still needs, and its own last hashes. The host compares every guest's hashes with
// its own, and each guest the host's: a frame whose hashes differ ends the session and names the frame, since from
// there the games differ.

#include "netplay_internal.h"
#include "udp.h"

#include <chrono>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

using namespace netplay;
using namespace netplay::detail;
using Clock = std::chrono::steady_clock;

namespace {
    constexpr uint32_t SESSION_MAGIC = 0x534E504D; // "MPNS"
    constexpr uint32_t SESSION_VERSION = 4;
    constexpr uint8_t KIND_HELLO = 0;
    constexpr uint8_t KIND_INPUTS = 1; // a guest's to the host
    constexpr uint8_t KIND_FRAMES = 2; // the host's to a guest
    constexpr int MAX_SEND = 24;
    constexpr int HASHES_SENT = 4;
    constexpr size_t MAX_IDENTITY = 800;
    constexpr size_t MAX_SAVE = 512; // a hello carries it whole
    using Inputs = std::array<Pad, MAX_SAMPLES>;
    using Whole = std::array<Inputs, PORTS>; // a frame's inputs, every player's

    bool hosting = false;
    int player = 0;
    int players = 2;
    int delay = 3;
    int64_t session_frames = -1;
    int timeout_ms = 15000;
    int drop_percent = 0;

    std::string identity;
    bool handshaken = false;

    std::vector<Inputs> local_inputs;   // by frame
    std::vector<uint64_t> local_hashes; // by frame
    uint64_t hashes_compared = 0;

    // The host as a guest sees it, or a guest as the host does.
    struct Remote {
        udp::Endpoint endpoint{};
        bool known = false;        // its address is known (the host's from the start; a guest's from its hello)
        bool identity_ok = false;  // its identity came, and matched
        bool saw_ours = false;     // it has this copy's identity
        uint32_t ack = 0;          // the first frame of this copy's it still needs
        // A guest's inputs, as the host keeps them, by frame.
        std::vector<Inputs> inputs;
        std::vector<uint8_t> have;
        uint32_t next = 0;         // the first frame of its inputs (on a guest: of whole frames) still missing
        std::deque<std::pair<uint32_t, uint64_t>> hashes; // its hashes, not yet compared
        uint32_t compare_from = 0;
        Clock::time_point heard = Clock::now();
    };
    Remote remotes[PORTS];          // the host's: by player, 1 on; a guest's: [0], the host
    std::vector<Whole> wholes;      // on a guest: the whole frames the host sent, by frame
    std::vector<uint8_t> whole_have;

    struct Writer {
        uint8_t buf[1400];
        size_t n = 0;
        void u8(uint8_t v) { if (n < sizeof(buf)) buf[n++] = v; }
        void u16(uint16_t v) { u8(uint8_t(v)); u8(uint8_t(v >> 8)); }
        void u32(uint32_t v) { for (int i = 0; i < 4; i++) u8(uint8_t(v >> (8 * i))); }
        void u64(uint64_t v) { u32(uint32_t(v)); u32(uint32_t(v >> 32)); }
        void pad(const Pad& p) { u16(p.button); u8(uint8_t(p.stick_x)); u8(uint8_t(p.stick_y)); u8(p.err_no); }
    };
    struct Reader {
        const uint8_t* buf;
        size_t n, at = 0;
        bool ok = true;
        uint8_t u8() { if (at >= n) { ok = false; return 0; } return buf[at++]; }
        uint16_t u16() { uint16_t lo = u8(); return uint16_t(lo | (u8() << 8)); }
        uint32_t u32() { uint32_t v = 0; for (int i = 0; i < 4; i++) v |= uint32_t(u8()) << (8 * i); return v; }
        uint64_t u64() { uint64_t lo = u32(); return lo | (uint64_t(u32()) << 32); }
        Pad pad() { Pad p; p.button = u16(); p.stick_x = int8_t(u8()); p.stick_y = int8_t(u8()); p.err_no = u8(); return p; }
    };

    // The save the session plays from: the host's own, which a guest has from the host's hello.
    std::vector<uint8_t> host_save;

    // What the session has said, for the player to see when it ends for a reason (Game's show_message).
    std::string said;

    // Prints a line of the session's to stderr, and keeps it.
    [[gnu::format(printf, 1, 2)]] void say(const char* format, ...) {
        char line[512];
        va_list args;
        va_start(args, format);
        vsnprintf(line, sizeof(line), format, args);
        va_end(args);
        fputs(line, stderr);
        said += strncmp(line, "[netplay] ", 10) == 0 ? line + 10 : line;
    }

    // Ends the program: 0 when a session of MP_NETPLAY_FRAMES has ended, 2 for a desync, 3 when a player went quiet
    // or never came, 4 when the copies differ, 5 when the session
    // couldn't start (no such host, a port in use). Any but 0 shows the player what the session said first.
    [[noreturn]] void end_session(int code) {
        fflush(stderr);
        if (code != 0 && game.show_message != nullptr) {
            game.show_message("Online play ended", said.c_str());
        }
        udp::close();
        std::_Exit(code);
    }

    void send(const Remote& to, const Writer& w) {
        if (!to.known || (drop_percent > 0 && rand() % 100 < drop_percent)) {
            return;
        }
        udp::send(to.endpoint, w.buf, w.n);
    }

    void header(Writer& w, uint8_t kind) {
        w.u32(SESSION_MAGIC);
        w.u32(SESSION_VERSION);
        w.u8(kind);
        w.u8(uint8_t(player));
    }

    void put_hashes(Writer& w) {
        uint32_t count = std::min<uint32_t>(uint32_t(local_hashes.size()), HASHES_SENT);
        w.u8(uint8_t(count));
        for (uint32_t i = 0; i < count; i++) {
            uint32_t f = uint32_t(local_hashes.size()) - count + i;
            w.u32(f);
            w.u64(local_hashes[f]);
        }
    }

    void send_hello(Remote& to) {
        Writer w;
        header(w, KIND_HELLO);
        w.u8(to.identity_ok ? 1 : 0);
        w.u8(uint8_t(players));
        size_t n = std::min(identity.size(), MAX_IDENTITY);
        w.u16(uint16_t(n));
        for (size_t i = 0; i < n; i++) {
            w.u8(uint8_t(identity[i]));
        }
        // The host's save, which the guests play from.
        w.u16(uint16_t(host_save.size()));
        for (uint8_t b : host_save) {
            w.u8(b);
        }
        send(to, w);
    }

    // The first frame some player's input is still missing for: on the host, how far the whole frames go.
    uint32_t wholes_ready() {
        uint32_t ready = uint32_t(local_inputs.size());
        for (int p = 1; p < players; p++) {
            ready = std::min(ready, remotes[p].next);
        }
        return ready;
    }

    Inputs input_of(int p, uint32_t f) {
        return p == 0 ? local_inputs[f] : remotes[p].inputs[f];
    }

    // A guest's to the host.
    void send_inputs() {
        Remote& host = remotes[0];
        Writer w;
        header(w, KIND_INPUTS);
        w.u32(host.next);
        uint32_t first = std::max<uint32_t>(host.ack, uint32_t(delay));
        uint32_t end = uint32_t(local_inputs.size());
        uint32_t count = end > first ? std::min<uint32_t>(end - first, MAX_SEND) : 0;
        w.u32(first);
        w.u8(uint8_t(count));
        for (uint32_t f = first; f < first + count; f++) {
            for (int i = 0; i < game.samples_per_frame; i++) {
                w.pad(local_inputs[f][i]);
            }
        }
        put_hashes(w);
        send(host, w);
    }

    // The host's to each guest.
    void send_frames() {
        uint32_t ready = wholes_ready();
        for (int p = 1; p < players; p++) {
            Remote& guest = remotes[p];
            Writer w;
            header(w, KIND_FRAMES);
            w.u32(guest.next);
            uint32_t first = std::max<uint32_t>(guest.ack, uint32_t(delay));
            uint32_t count = ready > first ? std::min<uint32_t>(ready - first, MAX_SEND) : 0;
            w.u32(first);
            w.u8(uint8_t(count));
            for (uint32_t f = first; f < first + count; f++) {
                for (int q = 0; q < players; q++) {
                    Inputs in = input_of(q, f);
                    for (int i = 0; i < game.samples_per_frame; i++) {
                        w.pad(in[i]);
                    }
                }
            }
            put_hashes(w);
            send(guest, w);
        }
    }

    void send_all() {
        if (hosting) {
            send_frames();
        } else {
            send_inputs();
        }
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

    void check_identity(Remote& from, int who, const std::string& theirs) {
        std::string ours = identity.substr(0, MAX_IDENTITY);
        if (theirs == ours) {
            from.identity_ok = true;
            return;
        }
        say("[netplay] player %d's copy differs from this one, so the two would play differently:\n", who + 1);
        auto a = lines(ours), b = lines(theirs);
        for (auto& [name, value] : a) {
            auto it = b.find(name);
            if (it == b.end()) {
                say("[netplay]   %s: here%s, theirs none\n", name.c_str(), value.c_str());
            } else if (it->second != value) {
                say("[netplay]   %s: here%s, theirs%s\n", name.c_str(), value.c_str(), it->second.c_str());
            }
        }
        for (auto& [name, value] : b) {
            if (a.find(name) == a.end()) {
                say("[netplay]   %s: none here, theirs%s\n", name.c_str(), value.c_str());
            }
        }
        for (int i = 0; i < 10; i++) {
            send_hello(from); // so the other copy can say what differs too
        }
        end_session(4);
    }

    void compare_hashes(Remote& from, int who) {
        while (!from.hashes.empty() && from.hashes.front().first < local_hashes.size()) {
            auto [f, h] = from.hashes.front();
            from.hashes.pop_front();
            if (local_hashes[f] != h) {
                say("[netplay] frame %u differs from player %d's: the session has desynced\n", f, who + 1);
                for (int i = 0; i < 20; i++) {
                    send_all(); // so the others see this copy's hash for the frame too
                }
                end_session(2);
            }
            hashes_compared++;
            from.compare_from = f + 1;
        }
    }

    void take_hashes(Reader& r, Remote& from, int who) {
        uint32_t count = r.u8();
        for (uint32_t i = 0; i < count && r.ok; i++) {
            uint32_t f = r.u32();
            uint64_t h = r.u64();
            if (r.ok && (from.hashes.empty() || f > from.hashes.back().first) && f >= from.compare_from) {
                from.hashes.emplace_back(f, h);
            }
        }
        compare_hashes(from, who);
    }

    // On the host, a guest's inputs.
    void take_inputs(Reader& r, Remote& guest, int who) {
        uint32_t ack = r.u32();
        uint32_t first = r.u32();
        uint32_t count = r.u8();
        if (!r.ok) {
            return;
        }
        guest.ack = std::max(guest.ack, ack);
        for (uint32_t f = first; f < first + count; f++) {
            Inputs in{};
            for (int i = 0; i < game.samples_per_frame; i++) {
                in[i] = r.pad();
            }
            if (!r.ok) {
                return;
            }
            if (guest.inputs.size() <= f) {
                guest.inputs.resize(f + 1);
                guest.have.resize(f + 1, 0);
            }
            guest.inputs[f] = in;
            guest.have[f] = 1;
        }
        while (guest.next < guest.have.size() && guest.have[guest.next]) {
            guest.next++;
        }
        take_hashes(r, guest, who);
    }

    // On a guest, the host's whole frames.
    void take_frames(Reader& r) {
        Remote& host = remotes[0];
        uint32_t ack = r.u32();
        uint32_t first = r.u32();
        uint32_t count = r.u8();
        if (!r.ok) {
            return;
        }
        host.ack = std::max(host.ack, ack);
        for (uint32_t f = first; f < first + count; f++) {
            Whole whole{};
            for (int q = 0; q < players; q++) {
                for (int i = 0; i < game.samples_per_frame; i++) {
                    whole[q][i] = r.pad();
                }
            }
            if (!r.ok) {
                return;
            }
            if (wholes.size() <= f) {
                wholes.resize(f + 1);
                whole_have.resize(f + 1, 0);
            }
            wholes[f] = whole;
            whole_have[f] = 1;
        }
        while (host.next < whole_have.size() && whole_have[host.next]) {
            host.next++;
        }
        take_hashes(r, host, 0);
    }

    // Takes in one packet, if one came within wait_ms. Returns whether one did.
    bool receive_packet(int wait_ms) {
        uint8_t buf[1500];
        udp::Endpoint from_endpoint;
        size_t n = udp::receive(buf, sizeof(buf), wait_ms, from_endpoint);
        if (n == 0) {
            return false;
        }
        Reader r{ buf, n };
        if (r.u32() != SESSION_MAGIC || r.u32() != SESSION_VERSION) {
            return false;
        }
        uint8_t kind = r.u8();
        int who = r.u8();
        if (!r.ok || who == player || who >= PORTS || (hosting ? who == 0 || who >= players : who != 0)) {
            return false;
        }
        Remote& from = remotes[hosting ? who : 0];
        if (hosting) {
            // A guest's address is where its packets come from: what its router shows the host.
            if (kind == KIND_HELLO) {
                if (!from.known) {
                    say("[netplay] player %d is at %s\n", who + 1, from_endpoint.text().c_str());
                }
                from.endpoint = from_endpoint;
                from.known = true;
            } else if (!from.known || from.endpoint != from_endpoint) {
                return false;
            }
        } else if (from_endpoint != from.endpoint) {
            return false;
        }
        from.heard = Clock::now();
        if (kind == KIND_HELLO) {
            bool saw = r.u8() != 0;
            int their_players = r.u8();
            uint16_t size = r.u16();
            std::string theirs;
            for (uint16_t i = 0; i < size && r.ok; i++) {
                theirs.push_back(char(r.u8()));
            }
            uint16_t save_size = r.u16();
            std::vector<uint8_t> save;
            for (uint16_t i = 0; i < save_size && r.ok; i++) {
                save.push_back(r.u8());
            }
            if (!r.ok) {
                return true;
            }
            if (their_players != players) {
                // The ports with a controller are the session's from power-on, before the handshake: every copy must
                // start with the same count.
                say("[netplay] player %d's session is of %d players, this one's of %d\n", who + 1,
                        their_players, players);
                for (int i = 0; i < 10; i++) {
                    send_hello(from);
                }
                end_session(4);
            }
            check_identity(from, who, theirs);
            if (!hosting) {
                host_save = std::move(save);
            }
            from.saw_ours = from.saw_ours || saw;
            if (handshaken && !saw) {
                send_hello(from); // it is still waiting for this copy's identity
            }
        } else if (kind == KIND_INPUTS && hosting) {
            if (from.identity_ok) {
                from.saw_ours = true;
            }
            take_inputs(r, from, who);
        } else if (kind == KIND_FRAMES && !hosting) {
            if (from.identity_ok) {
                from.saw_ours = true;
            }
            take_frames(r);
        }
        return true;
    }

    bool everyone_ready() {
        if (!hosting) {
            return remotes[0].identity_ok && remotes[0].saw_ours;
        }
        for (int p = 1; p < players; p++) {
            if (!remotes[p].identity_ok || !remotes[p].saw_ours) {
                return false;
            }
        }
        return true;
    }

    void hello_everyone() {
        if (hosting) {
            for (int p = 1; p < players; p++) {
                send_hello(remotes[p]);
            }
        } else {
            send_hello(remotes[0]);
        }
    }

    void handshake() {
        identity = game.identity != nullptr ? game.identity() : std::string();
        identity += "delay: " + std::to_string(delay) + " frames\n"; // a frame's inputs are its players' from as far back
        if (hosting && game.save != nullptr) {
            host_save = game.save();
            if (host_save.size() > MAX_SAVE) {
                say("[netplay] the save is %zu bytes, more than a hello carries\n", host_save.size());
                end_session(5);
            }
        }
        if (hosting) {
            say("[netplay] waiting for %d players to join\n", players - 1);
        } else {
            say("[netplay] waiting for the host\n");
        }
        auto until = Clock::now() + std::chrono::minutes(5);
        auto next_hello = Clock::now();
        while (!everyone_ready()) {
            auto now = Clock::now();
            if (now >= next_hello) {
                hello_everyone();
                next_hello = now + std::chrono::milliseconds(50);
            }
            if (now > until) {
                say(hosting ? "[netplay] not every player joined\n" : "[netplay] the host never answered\n");
                end_session(3);
            }
            receive_packet(16);
        }
        // A last few, so the others know this copy has theirs even if one is lost.
        for (int i = 0; i < 3; i++) {
            hello_everyone();
        }
        handshaken = true;
        say("[netplay] every copy matches: the session starts with %d players\n", players);
    }

    // Whether the frame's inputs from everyone are here.
    bool frame_ready() {
        return hosting ? wholes_ready() > frame_no : remotes[0].next > frame_no;
    }

    // Whether every other copy has all it needs of this one's to play the session's frames.
    bool others_have_all() {
        uint32_t end = uint32_t(session_frames);
        if (!hosting) {
            return remotes[0].ack >= end;
        }
        for (int p = 1; p < players; p++) {
            if (remotes[p].ack < end) {
                return false;
            }
        }
        return true;
    }

    // The copy that has been silent longest, while a frame waits.
    const Remote& quietest(int& who) {
        int first = hosting ? 1 : 0, last = hosting ? players : 1;
        who = first;
        for (int p = first; p < last; p++) {
            if (remotes[p].heard < remotes[who].heard) {
                who = p;
            }
        }
        return remotes[who];
    }
}

bool netplay::detail::open_session(const char* peer, int host_players) {
    hosting = host_players > 0;
    if (const char* d = getenv("MP_NETPLAY_DELAY")) delay = std::max(1, atoi(d));
    if (const char* f = getenv("MP_NETPLAY_FRAMES")) session_frames = strtoll(f, nullptr, 10);
    if (const char* t = getenv("MP_NETPLAY_TIMEOUT_MS")) timeout_ms = atoi(t);
    if (const char* d = getenv("MP_NETPLAY_DROP")) drop_percent = atoi(d);
    const char* port_text = getenv("MP_NETPLAY_PORT");
    uint16_t port = uint16_t(atoi(port_text != nullptr ? port_text : (hosting ? "7845" : "0")));
    if (hosting) {
        player = 0;
        players = std::clamp(host_players, 2, PORTS);
    } else {
        const char* p = getenv("MP_NETPLAY_PLAYER");
        const char* n = getenv("MP_NETPLAY_PLAYERS");
        player = p != nullptr ? atoi(p) : 1;
        players = n != nullptr ? atoi(n) : 2;
        if (players < 2 || players > PORTS || player < 1 || player >= players) {
            say("[netplay] a guest's MP_NETPLAY_PLAYERS is 2 to 4 and its MP_NETPLAY_PLAYER 1 to one less\n");
            end_session(5);
        }
        if (!udp::resolve(peer, remotes[0].endpoint)) {
            say("[netplay] couldn't find the host at %s (an address and port, like 203.0.113.5:7845)\n", peer);
            end_session(5);
        }
        remotes[0].known = true;
    }
    if (!udp::open(port)) {
        say("[netplay] couldn't open UDP port %u: is another copy of the game using it?\n", unsigned(port));
        end_session(5);
    }
    local_inputs.assign(size_t(delay), Inputs{});
    for (Remote& r : remotes) {
        r.inputs.assign(size_t(delay), Inputs{});
        r.have.assign(size_t(delay), 1);
        r.next = uint32_t(delay);
    }
    wholes.assign(size_t(delay), Whole{});
    whole_have.assign(size_t(delay), 1);
    // The ports with a controller, from power-on: one for each player.
    connected_mask = (1u << players) - 1;
    if (hosting) {
        say("[netplay] hosting a session of %d players on UDP port %u, %d frames' delay\n", players,
                unsigned(port), delay);
    } else {
        say("[netplay] joining the session at %s as player %d, %d frames' delay\n", peer, player + 1, delay);
    }
    return true;
}

Frame netplay::detail::session_frame(uint64_t hash) {
    if (!handshaken) {
        handshake();
    }
    if (session_frames >= 0 && int64_t(frame_no) >= session_frames) {
        // Stay until the others have this copy's part of every frame they play, then end.
        auto until = Clock::now() + std::chrono::seconds(3);
        while (!others_have_all() && Clock::now() < until) {
            send_all();
            receive_packet(16);
        }
        say("[netplay] played %" PRIu64 " frames: %" PRIu64 " hashes compared, every one matched\n",
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

    while (receive_packet(0)) {
    }
    send_all();
    bool told = false;
    while (!frame_ready()) {
        if (receive_packet(16)) {
            continue;
        }
        send_all();
        int who = 0;
        auto silent = Clock::now() - quietest(who).heard;
        if (!told && silent > std::chrono::seconds(1)) {
            say("[netplay] frame %" PRIu64 ": waiting for player %d\n", frame_no, who + 1);
            told = true;
        }
        if (silent > std::chrono::milliseconds(timeout_ms)) {
            say("[netplay] frame %" PRIu64 ": player %d stopped answering\n", frame_no, who + 1);
            end_session(3);
        }
    }
    if (hosting) {
        send_frames(); // the frame is whole: the guests can have it now
    }

    Frame fr;
    fr.connected = uint8_t(connected_mask);
    for (int q = 0; q < players; q++) {
        Inputs in = hosting ? input_of(q, uint32_t(frame_no)) : wholes[frame_no][q];
        for (int i = 0; i < game.samples_per_frame; i++) {
            fr.samples[i][q] = in[i];
        }
    }
    return fr;
}

void netplay::detail::start_session() {
    if (handshaken) {
        return;
    }
    handshake();
    if (!hosting && game.use_save != nullptr) {
        game.use_save(host_save);
    }
}
