// A session between copies of the game: delay-based lockstep over UDP for two to four players, through a host. The
// host plays port 1 and listens on its port; each guest knows only the host's address, plays the port the host gives
// it (the first free, as it joins) with its own first pad, and sends only to the host (SessionSettings,
// netplay_core.h). A frame's inputs are every player's pad as each copy sampled it the session's delay in frames
// earlier: each guest sends the host its own, the host puts each frame together once it has every player's, and sends the whole frame to
// every guest. So every copy runs the same frames on the same inputs, and each game turns them into the same state (a
// recording's replay tests that it does). Only the host needs a port others can reach. The first DELAY frames have no
// inputs from anyone.
//
// It starts with a handshake, before the game's entry (netplay::start): each guest sends the host the game's identity
// (netplay_core.h's Game) until the host has it and has answered with its own, the guest's port, how many play, and
// the delay, which it chooses from the round trips the hellos' clocks measure. The session starts once the host has
// every guest's. Identities that differ end the session, naming the lines that differ: a different ROM, build, mod or option would
// play differently from the same inputs. The host's hello carries its save too, and each guest plays from it in place
// of its own (Game's save and use_save), since a different save plays differently. netplay::start runs the handshake
// before the game starts, so a guest has the host's save before the game reads it.
//
// From the launcher (lobby_open, netplay_core.h) the handshake runs before the game starts instead, on a thread of its
// own, as a lobby: the host takes guests as they come (up to four players in all) and says how many play only when its
// player presses Start, which ends the lobby; until then its hellos say 0 players, a guest that goes quiet or says
// goodbye (KIND_BYE) leaves the lobby and the guests after it move up a player, and a guest whose copy differs is turned
// away. Every hello also carries its player's name and whether they're ready, and the host's carry its list of the
// players (each one's name, whether they're ready, and their round trip to the host), for the lobby's screen; the host
// starts only once every player, its own too, is ready. A lobby that ends early (the host closed it, a copy differs, the host never answered) doesn't end the program
// as a session does: the lobby's status says why, for the launcher to show.
//
// Then the packets, each frame and again every 16 ms while a frame waits. A guest's to the host: its inputs from the
// first frame the host hasn't acknowledged, the first whole frame it still needs, and the hashes of the game state it
// made for its last frames. The host's to each guest: the whole frames from the first that guest still needs, the
// first of that guest's inputs it still needs, and its own last hashes. The host compares every guest's hashes with
// its own, and each guest the host's: a frame whose hashes differ ends the session and names the frame, since from
// there the games differ.

#include "netplay_internal.h"
#include "udp.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace netplay;
using namespace netplay::detail;
using Clock = std::chrono::steady_clock;

namespace {
    constexpr uint32_t SESSION_MAGIC = 0x534E504D; // "MPNS"
    constexpr uint32_t SESSION_VERSION = 6;
    constexpr uint8_t KIND_HELLO = 0;
    constexpr uint8_t KIND_INPUTS = 1; // a guest's to the host
    constexpr uint8_t KIND_FRAMES = 2; // the host's to a guest
    constexpr uint8_t KIND_BYE = 3;    // a copy leaving the lobby
    constexpr int MAX_SEND = 24;
    constexpr int HASHES_SENT = 4;
    constexpr size_t MAX_IDENTITY = 800;
    constexpr size_t MAX_SAVE = 512; // a hello carries it whole
    constexpr uint8_t UNASSIGNED = 0xFF; // a guest's player before the host gives it one
    using Inputs = std::array<Pad, MAX_SAMPLES>;
    using Whole = std::array<Inputs, PORTS>; // a frame's inputs, every player's

    bool hosting = false;
    int player = 0;
    int players = 2;
    int delay = 3;
    bool delay_forced = false; // MP_NETPLAY_DELAY, on the host: no measuring
    bool delay_known = false;  // the host has chosen the session's delay (a guest: and sent it)
    int64_t session_frames = -1;
    int timeout_ms = 15000;
    int drop_percent = 0;

    std::string identity;
    bool handshaken = false;
    bool save_used = false;      // a guest has played from the host's save (start_session)
    bool record_opened = false;  // the session's record (at the first frame, after a lobby)

    // The lobby (lobby_open): the handshake on a thread of its own, before the game starts.
    struct LobbyEnded {
        int code;
    };
    bool in_lobby = false;       // the lobby's thread runs the handshake: end_session throws LobbyEnded
    bool lobby_started = false;  // the host's player pressed Start: how many play is settled
    std::atomic<bool> lobby_start_wanted{ false };
    std::atomic<bool> lobby_cancel{ false };
    std::thread lobby_thread;
    std::mutex status_mutex;
    LobbyStatus status;          // under status_mutex
    size_t reason_from = 0;      // where in said the lines of the lobby's last step begin: why it failed, if it did
    int jitter_ms = 0; // MP_NETPLAY_JITTER_MS: a stall of up to this long before one frame in four, for the tests

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
        // Round trips, from the hellos: each carries its sender's clock and the last one it had from the other.
        uint64_t their_clock = 0;
        uint64_t their_clock_at = 0; // this copy's clock when their_clock came
        std::vector<double> rtts_ms; // the latest, at most 32
        // The lobby: whether it has readied up, and its player's name.
        bool ready = false;
        std::string name;
    };
    // On a guest: the lobby's players as the host last listed them.
    struct Seat {
        bool present = false, ready = false;
        int ping_ms = -1;
        std::string name;
    };
    Seat roster[PORTS];
    constexpr size_t MAX_NAME = 16;
    std::string local_name;
    std::atomic<bool> local_ready{ false };
    std::string peer_text;

    int slot_for(const udp::Endpoint& from);
    void drop_guest(int p);

    uint64_t clock_us() {
        return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count()) | 1;
    }

    double median_rtt(const Remote& r) {
        std::vector<double> v = r.rtts_ms;
        std::sort(v.begin(), v.end());
        return v.empty() ? 0.0 : v[v.size() / 2];
    }
    Remote remotes[PORTS];          // the host's: by player, 1 on; a guest's: [0], the host
    std::vector<Whole> wholes;      // on a guest: the whole frames the host sent, by frame
    std::vector<uint8_t> whole_have;

    struct Writer {
        uint8_t buf[1472]; // a UDP datagram that fits a 1500-byte link
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
        log_session_line(line);
        said += strncmp(line, "[netplay] ", 10) == 0 ? line + 10 : line;
    }

    // Ends the program: 0 when a session of MP_NETPLAY_FRAMES has ended, 2 for a desync, 3 when a player went quiet
    // or never came, 4 when the copies differ, 5 when the session
    // couldn't start (no such host, a port in use). Any but 0 shows the player what the session said first.
    [[noreturn]] void end_session(int code) {
        if (in_lobby) {
            throw LobbyEnded{ code }; // the lobby's thread says why and goes back to the launcher
        }
        fflush(stderr);
        close_session_record();
        if (code != 0 && game.show_message != nullptr) {
            std::string text = said;
            if (code == 2 && !session_record_folder().empty()) {
                text += "\nThis session's record, for whoever is fixing it, is in:\n" + session_record_folder();
            }
            game.show_message("Online play ended", text.c_str());
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
        w.u8(player >= 0 ? uint8_t(player) : UNASSIGNED);
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

    void put_name(Writer& w, const std::string& name) {
        size_t n = std::min(name.size(), MAX_NAME);
        w.u8(uint8_t(n));
        for (size_t i = 0; i < n; i++) {
            w.u8(uint8_t(name[i]));
        }
    }

    std::string take_name(Reader& r) {
        size_t n = std::min<size_t>(r.u8(), MAX_NAME);
        std::string name;
        for (size_t i = 0; i < n && r.ok; i++) {
            char c = char(r.u8());
            name.push_back(c >= 32 && c != 127 ? c : '?');
        }
        return name;
    }

    void send_hello(Remote& to) {
        Writer w;
        header(w, KIND_HELLO);
        // Whether this copy has the other's identity (a guest: and the host's delay), so it may start.
        w.u8(to.identity_ok && (hosting || (delay_known && players > 0)) ? 1 : 0);
        // How many play: 0 from a host whose lobby is still open.
        w.u8(uint8_t(hosting && in_lobby && !lobby_started ? 0 : players));
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
        // For the round trip: this copy's clock, the other's last and how long ago it came. And the host's delay once it has chosen it.
        w.u64(clock_us());
        w.u64(to.their_clock);
        w.u32(to.their_clock != 0 ? uint32_t(clock_us() - to.their_clock_at) : 0); // how long this copy held it
        w.u8(uint8_t(hosting && delay_known ? delay : 0));
        // The host's to a guest: the player it is.
        w.u8(uint8_t(hosting ? int(&to - remotes) : 0));
        // The lobby: this copy's ready and name, and the host's list of everyone's.
        w.u8(local_ready ? 1 : 0);
        put_name(w, local_name);
        if (hosting) {
            for (int p = 0; p < PORTS; p++) {
                const Remote& r = remotes[p];
                bool present = p == 0 || r.known;
                w.u8(uint8_t((present ? 1 : 0) | ((p == 0 ? bool(local_ready) : r.ready) ? 2 : 0)));
                w.u16(uint16_t(p == 0 || r.rtts_ms.empty() ? 0xFFFF : std::min(65534.0, median_rtt(r))));
                put_name(w, p == 0 ? local_name : r.name);
            }
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
        if (hosting && in_lobby) {
            drop_guest(who); // the lobby stays open for the others
            return;
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
        if (hosting && r.ok && kind == KIND_BYE) {
            if (!handshaken) {
                for (int p = 1; p < players; p++) {
                    if (remotes[p].known && remotes[p].endpoint == from_endpoint) {
                        say("[netplay] player %d left\n", p + 1);
                        drop_guest(p);
                        break;
                    }
                }
            }
            return true;
        }
        if (!hosting && r.ok && kind == KIND_BYE) {
            if (from_endpoint == remotes[0].endpoint && !handshaken) {
                say("[netplay] the host closed the lobby\n");
                end_session(3);
            }
            return true;
        }
        if (hosting && r.ok && kind == KIND_HELLO && (who == UNASSIGNED || !handshaken)) {
            // Before the session starts a guest is the player its address has (the guests move up when one leaves).
            who = slot_for(from_endpoint);
            if (who < 0) {
                return false; // the session is full
            }
        }
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
            uint64_t their_clock = r.u64();
            uint64_t echo = r.u64();
            uint32_t held = r.u32();
            int their_delay = r.u8();
            int slot = r.u8();
            bool their_ready = r.u8() != 0;
            std::string their_name = take_name(r);
            Seat their_roster[PORTS];
            if (!hosting) {
                for (int p = 0; p < PORTS; p++) {
                    uint8_t flags = r.u8();
                    uint16_t ping = r.u16();
                    their_roster[p] = Seat{ (flags & 1) != 0, (flags & 2) != 0, ping == 0xFFFF ? -1 : int(ping), take_name(r) };
                }
            }
            if (!r.ok) {
                return true;
            }
            from.ready = their_ready;
            from.name = their_name;
            if (!hosting) {
                for (int p = 0; p < PORTS; p++) {
                    roster[p] = their_roster[p];
                }
            }
            if (!hosting && !handshaken) {
                // The host's hellos: this copy's player (which moves up if one before it leaves the lobby), and how
                // many play once the host has said (0 while its lobby is open).
                if (slot >= 1 && slot < PORTS && slot != player) {
                    player = slot;
                    say("[netplay] the host has made this copy player %d\n", player + 1);
                }
                if (players == 0 && their_players >= 2 && their_players <= PORTS && player > 0 && player < their_players) {
                    players = their_players;
                    say("[netplay] the session is of %d players\n", players);
                }
            }
            if (their_players != players && their_players != 0) {
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
            from.their_clock = their_clock;
            from.their_clock_at = clock_us();
            if (echo != 0 && clock_us() - echo > held) {
                from.rtts_ms.push_back(double(clock_us() - echo - held) / 1000.0);
                if (from.rtts_ms.size() > 32) {
                    from.rtts_ms.erase(from.rtts_ms.begin());
                }
            }
            if (!hosting) {
                host_save = std::move(save);
                if (their_delay > 0 && !delay_known) {
                    delay = their_delay;
                    delay_known = true;
                }
            }
            from.saw_ours = from.saw_ours || saw;
            if (handshaken && !saw) {
                send_hello(from); // it is still waiting for this copy's identity
            }
        } else if (kind == KIND_INPUTS && hosting) {
            if (from.identity_ok) {
                from.saw_ours = true;
            }
            if (handshaken) { // its buffers are made once the handshake ends; the guest sends again
                take_inputs(r, from, who);
            }
        } else if (kind == KIND_FRAMES && !hosting) {
            if (from.identity_ok && delay_known) {
                from.saw_ours = true;
            }
            if (handshaken) {
                take_frames(r);
            }
        }
        return true;
    }

    bool everyone_ready() {
        if (!hosting) {
            return remotes[0].identity_ok && remotes[0].saw_ours && delay_known && players > 0;
        }
        if (in_lobby && !lobby_started) {
            return false;
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

    // On the host: the player a guest that hasn't one yet is, by where its packets come from: the one it was already
    // given, or the first free one.
    int slot_for(const udp::Endpoint& from) {
        for (int p = 1; p < players; p++) {
            if (remotes[p].known && remotes[p].endpoint == from) {
                return p;
            }
        }
        if (in_lobby && lobby_started) {
            return -1; // the host has started: no one else joins
        }
        for (int p = 1; p < players; p++) {
            if (!remotes[p].known) {
                return p;
            }
        }
        return -1;
    }

    // On the host, in its lobby: a guest gone, and the guests after it each a player up, so the players stay one to
    // however many there are.
    void drop_guest(int p) {
        for (int q = p; q + 1 < PORTS; q++) {
            remotes[q] = remotes[q + 1];
        }
        remotes[PORTS - 1] = Remote{};
    }

    // On the host, once every guest has answered a few hellos: the delay, from the longest round trip. A guest's
    // frame waits for its inputs to reach the host and the whole frame to come back, a round trip, so the delay covers
    // the round trip and some for its jitter.
    void choose_delay() {
        double worst = 0;
        for (int p = 1; p < players; p++) {
            if (!remotes[p].identity_ok || remotes[p].rtts_ms.size() < 8) {
                return;
            }
            worst = std::max(worst, median_rtt(remotes[p]));
        }
        if (!delay_forced) {
            delay = std::clamp(int(std::ceil((worst + 35.0) / (1000.0 / 30.0))), 2, 15);
        }
        delay_known = true;
        say("[netplay] round trip %.0f ms: %d frames' delay\n", worst, delay);
    }

    // How long frames waited for the others, logged every 10 seconds (the session's log, in its record).
    double waited_ms = 0, longest_wait_ms = 0;
    int frames_waited = 0;
    void note_wait(double ms) {
        if (ms > 1.0) {
            waited_ms += ms;
            longest_wait_ms = std::max(longest_wait_ms, ms);
            frames_waited++;
        }
        if (frame_no % 300 == 299) {
            char line[200];
            snprintf(line, sizeof(line), "[netplay] frames to %" PRIu64 ": %d waited, %.0f ms in all, the longest %.0f ms\n",
                    frame_no, frames_waited, waited_ms, longest_wait_ms);
            log_session_line(line);
            waited_ms = longest_wait_ms = 0;
            frames_waited = 0;
        }
    }

    // The buffers, once the delay is known: the first frames have no inputs from anyone.
    void prepare_buffers() {
        // The ports with a controller, from power-on (the game's entry comes after the handshake): one for each player.
        connected_mask = (1u << players) - 1;
        local_inputs.assign(size_t(delay), Inputs{});
        for (Remote& r : remotes) {
            r.inputs.assign(size_t(delay), Inputs{});
            r.have.assign(size_t(delay), 1);
            r.next = uint32_t(delay);
        }
        wholes.assign(size_t(delay), Whole{});
        whole_have.assign(size_t(delay), 1);
    }

    void prepare_handshake() {
        identity = game.identity != nullptr ? game.identity() : std::string();
        if (hosting && game.save != nullptr) {
            host_save = game.save();
            if (host_save.size() > MAX_SAVE) {
                say("[netplay] the save is %zu bytes, more than a hello carries\n", host_save.size());
                end_session(5);
            }
        }
    }

    void finish_handshake() {
        // A last few, so the others know this copy has theirs even if one is lost.
        for (int i = 0; i < 3; i++) {
            hello_everyone();
        }
        prepare_buffers();
        handshaken = true;
        say("[netplay] every copy matches: the session starts with %d players\n", players);
    }

    void handshake() {
        prepare_handshake();
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
            if (hosting && !delay_known) {
                choose_delay();
            }
        }
        finish_handshake();
    }

    void set_status(LobbyStatus::Phase phase) {
        std::lock_guard lock{ status_mutex };
        status.phase = phase;
        status.host = hosting;
        status.player = player;
        for (int p = 0; p < PORTS; p++) {
            LobbySeat& seat = status.seats[p];
            if (hosting) {
                const Remote& r = remotes[p];
                seat.present = p == 0 || r.known;
                seat.ready = p == 0 ? bool(local_ready) : r.ready;
                seat.ping_ms = p == 0 || r.rtts_ms.empty() ? -1 : int(std::lround(median_rtt(r)));
                seat.name = p == 0 ? local_name : r.name;
                seat.address = p == 0 || !r.known ? std::string() : r.endpoint.text();
            } else {
                seat = LobbySeat{ roster[p].present, roster[p].ready, roster[p].ping_ms, roster[p].name, "" };
                if (p == player) {
                    seat.present = true;
                    seat.ready = local_ready;
                    seat.name = local_name;
                }
            }
        }
        status.peer = peer_text;
        status.said = said;
        status.reason = phase == LobbyStatus::Failed ? said.substr(std::min(reason_from, said.size())) : std::string();
    }

    // The lobby's thread: the handshake, with the guests as they come, until the host's player presses Start (or a
    // guest's host does), or the lobby closes.
    void run_lobby() {
        try {
            prepare_handshake();
            say(hosting ? "[netplay] the lobby is open\n" : "[netplay] waiting for the host\n");
            auto next_hello = Clock::now();
            auto opened = Clock::now();
            while (!everyone_ready()) {
                reason_from = said.size();
                if (lobby_cancel) {
                    for (int i = 0; i < 5; i++) {
                        Writer w;
                        header(w, KIND_BYE);
                        if (hosting) {
                            for (int p = 1; p < PORTS; p++) {
                                send(remotes[p], w);
                            }
                        } else {
                            send(remotes[0], w);
                        }
                    }
                    udp::close();
                    set_status(LobbyStatus::Closed);
                    return;
                }
                auto now = Clock::now();
                if (hosting && lobby_start_wanted && !lobby_started) {
                    int n = 1;
                    bool all_ready = local_ready;
                    while (n < PORTS && remotes[n].known) {
                        all_ready = all_ready && remotes[n].ready;
                        n++;
                    }
                    if (n >= 2 && all_ready) {
                        players = n;
                        lobby_started = true;
                        say("[netplay] starting with %d players\n", players);
                    }
                    lobby_start_wanted = false;
                }
                if (now >= next_hello) {
                    if (hosting) {
                        for (int p = 1; p < PORTS; p++) {
                            send_hello(remotes[p]);
                        }
                    } else {
                        send_hello(remotes[0]);
                    }
                    next_hello = now + std::chrono::milliseconds(50);
                }
                receive_packet(16);
                if (hosting) {
                    // A guest gone quiet has left (its game closed, its network went).
                    for (int p = 1; p < PORTS; p++) {
                        if (remotes[p].known && now - remotes[p].heard > std::chrono::seconds(4)) {
                            say("[netplay] player %d stopped answering and left\n", p + 1);
                            drop_guest(p);
                            if (lobby_started && p < players) {
                                end_session(3);
                            }
                        }
                    }
                    if (lobby_started && !delay_known) {
                        choose_delay();
                    }
                } else {
                    bool heard = player >= 0;
                    auto silent = now - (heard ? remotes[0].heard : opened);
                    if (silent > std::chrono::seconds(heard ? 6 : 20)) {
                        say(heard ? "[netplay] the host stopped answering\n"
                                  : "[netplay] the host never answered: check the address, and that the host's port "
                                    "is open to you\n");
                        end_session(3);
                    }
                }
                set_status(LobbyStatus::Waiting);
            }
            finish_handshake();
            set_status(LobbyStatus::Ready);
        } catch (const LobbyEnded&) {
            udp::close();
            set_status(LobbyStatus::Failed);
        }
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

bool netplay::detail::open_session(const SessionSettings& settings) {
    hosting = settings.host;
    if (const char* d = getenv("MP_NETPLAY_DELAY")) {
        delay = std::clamp(atoi(d), 1, 15); // the host's, for every player; a guest's is ignored
        delay_forced = true;
    }
    if (const char* f = getenv("MP_NETPLAY_FRAMES")) session_frames = strtoll(f, nullptr, 10);
    if (const char* t = getenv("MP_NETPLAY_TIMEOUT_MS")) timeout_ms = atoi(t);
    if (const char* d = getenv("MP_NETPLAY_DROP")) drop_percent = atoi(d);
    if (const char* j = getenv("MP_NETPLAY_JITTER_MS")) jitter_ms = atoi(j);
    uint16_t port = settings.port;
    if (hosting) {
        player = 0;
        players = std::clamp(settings.players, 2, PORTS);
    } else {
        // The host says which player this copy is and how many play, unless the settings already say.
        player = settings.player > 0 ? settings.player : -1;
        players = settings.player > 0 ? settings.players : 0;
        if (player > 0 && (players < 2 || players > PORTS || player >= players)) {
            say("[netplay] a guest's players are 2 to 4, and its player number 2 to that many\n");
            end_session(5);
        }
        std::string peer = settings.peer;
        if (peer.find(':') == std::string::npos) {
            peer += ":7845";
        }
        if (!udp::resolve(peer, remotes[0].endpoint)) {
            say("[netplay] couldn't find the host at %s (an address, like 203.0.113.5 or 203.0.113.5:7845)\n", peer.c_str());
            end_session(5);
        }
        remotes[0].known = true;
    }
    if (!udp::open(port)) {
        say("[netplay] couldn't open UDP port %u: is another copy of the game using it?\n", unsigned(port));
        end_session(5);
    }
    if (hosting && in_lobby) {
        say("[netplay] hosting on UDP port %u\n", unsigned(port));
    } else if (hosting) {
        say("[netplay] hosting a session of %d players on UDP port %u\n", players, unsigned(port));
    } else {
        say("[netplay] joining the session at %s\n", settings.peer.c_str());
    }
    return true;
}

Frame netplay::detail::session_frame(uint64_t hash) {
    if (!handshaken) {
        handshake();
    }
    if (!record_opened) {
        record_opened = true;
        open_session_record(player, host_save, said);
        for (Remote& r : remotes) {
            r.heard = Clock::now(); // the time from the lobby to the game's first frame doesn't count as silence
        }
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
    if (jitter_ms > 0 && rand() % 4 == 0) {
        // A stall, as a busy computer or a late packet makes one, for the tests.
        std::this_thread::sleep_for(std::chrono::milliseconds(rand() % jitter_ms));
    }

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
    auto wait_start = Clock::now();
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
    note_wait(std::chrono::duration<double, std::milli>(Clock::now() - wait_start).count());
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
    if (!handshaken) {
        handshake();
    }
    if (!hosting && game.use_save != nullptr && !save_used) {
        save_used = true;
        game.use_save(host_save);
    }
}

bool netplay::detail::lobby_begin(const SessionSettings& settings) {
    // As new: a lobby may open again after one that closed.
    for (Remote& r : remotes) {
        r = Remote{};
    }
    said.clear();
    host_save.clear();
    handshaken = delay_known = lobby_started = save_used = record_opened = false;
    lobby_start_wanted = lobby_cancel = local_ready = false;
    for (Seat& seat : roster) {
        seat = Seat{};
    }
    local_name = settings.name.substr(0, MAX_NAME);
    peer_text = settings.peer;
    delay = 3;
    in_lobby = true;
    reason_from = 0;
    try {
        open_session(settings);
    } catch (const LobbyEnded&) {
        set_status(LobbyStatus::Failed);
        return false;
    }
    if (hosting) {
        players = PORTS; // the guests' places, until Start says how many play
        std::lock_guard lock{ status_mutex };
        status.share.clear();
        for (const std::string& a : udp::local_addresses()) {
            status.share.push_back(a + ":" + std::to_string(settings.port != 0 ? settings.port : 7845));
        }
    }
    set_status(LobbyStatus::Waiting);
    lobby_thread = std::thread(run_lobby);
    return true;
}

LobbyStatus netplay::lobby_status() {
    std::lock_guard lock{ status_mutex };
    return status;
}

void netplay::lobby_set_ready(bool ready) {
    local_ready = ready;
}

void netplay::lobby_start() {
    lobby_start_wanted = true;
}

void netplay::detail::lobby_end(bool play) {
    if (!play) {
        lobby_cancel = true;
    }
    if (lobby_thread.joinable()) {
        lobby_thread.join();
    }
    if (!play) {
        udp::close(); // a lobby that had finished (Ready) and was left all the same
    }
    in_lobby = false;
}
