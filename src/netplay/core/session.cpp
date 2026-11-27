// A session between copies of the game: delay-based lockstep over UDP for two to four players, through a host. The
// host plays port 1 and listens on its port; each guest knows only the host's address, plays the port the host gives
// it (the first free, as it joins) with its own first pad, and sends only to the host (SessionSettings,
// netplay_core.h). A frame's inputs are every player's pad as each copy sampled it the session's delay in frames
// earlier: each guest sends the host its own, the host puts each frame together once it has every player's, and sends the whole frame to
// every guest. So every copy runs the same frames on the same inputs, and each game turns them into the same state (a
// recording's replay tests that it does). Only the host needs a port others can reach. The first frames, as many as
// the delay the session starts with, have no inputs from anyone.
//
// The delay moves as the session plays (adjust_delay, push_local): the host keeps measuring each guest's round trip
// from the clocks its packets carry, and how long each copy's frames waited, and sends the delay it wants with the
// whole frames. It goes up at once, when the round trips grow or frames wait, and down a frame at a time, after ten
// seconds in which it could have, since a delay that stays put feels better than the lowest one. Each copy moves its
// own delay to it by sampling its pad for two frames once, or for none, so no two copies need agree on when: a
// frame's inputs are still whatever each player's copy sent. MP_NETPLAY_DELAY on the host holds it where it says.
//
// It starts with a handshake, before the game's entry (netplay::start): each guest sends the host the game's identity
// (netplay_core.h's Game) until the host has it and has answered with its own, the guest's port, how many play, and
// the delay, which it chooses from the round trips the hellos' clocks measure. The session starts once the host has
// every guest's. Identities that differ end the session, naming the lines that differ: a different ROM, build, mod or option would
// play differently from the same inputs. The host's hello carries its save too, and each guest plays from it in place
// of its own (Game's save and use_save), since a different save plays differently. netplay::start runs the handshake
// before the game starts, so a guest has the host's save before the game reads it. The hellos also carry the party's
// id, which the host chooses, and whether the host resumes its last party (which one: its frame and hash), and each
// guest's answer, whether its copy saved the same; the session starts only once every guest's has.
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
// Then the packets, each frame and again every 16 ms while a frame waits, taken in by a thread of the session's own as
// they come: the host sends the guests a frame as soon as the last of its inputs comes, not at its own next frame. A guest's to the host: its inputs from the
// first frame the host hasn't acknowledged, the first whole frame it still needs, and the hashes of the game state it
// made for its last frames. The host's to each guest: the whole frames from the first that guest still needs, the
// first of that guest's inputs it still needs, and its own last hashes. The host compares every guest's hashes with
// its own, and each guest the host's: a frame whose hashes differ is a desync, since from there the games differ: the
// two copies swap that frame's hashes of each 4 KB page (KIND_DESYNC), play on to the same frame and write RDRAM there
// to the session's record, so the record says where the games went apart. Then the copies resync (start_resync): the
// host has every copy take up its last party save after the same frame, which it holds back until each guest has the
// save (KIND_RESYNC, KIND_RESYNC_ACK, KIND_SAVE_PIECE), and the hashes compare again from the frame after.
//
// A guest that leaves (its goodbye as its window closes, or quiet for drop_ms) doesn't end the host's session: the
// frames wait for it while the host's player chooses (guest_gone, Game's ask_left) and the other guests are told why
// (KIND_NOTICE). One that was only quiet and is heard again plays on. Computer plays: from the first frame of its inputs
// the host lacks, they're empty and the frame's left mask says so, which the game reads to have a computer player take
// that player's place (leave_session, Frame's left). Back to lobby: every copy goes back to the game's menu (Game's
// back_to_menu), the host to its lobby again and the guests joining it. A player who left can join again (rejoin_slot):
// the host's hello says the session is in progress, the joining copy's game plays for itself (joining_frame) until it
// has the host's party save, and the host has every copy resync to that save (check_rejoins), the joining copy taking
// it up at the first frame its game could be saved at (join_now) and the player given back their character (the
// resync's ports given back). A host that leaves ends its guests'.
// Each whole frame also carries the VIs it took on the host (Frame's vis), for the game's sound to keep real time.
//
// Through a netplay server (SessionSettings' via_server; udp.h's relay_connect) the same packets go by way of the
// server, which passes them on: the host and the guests are its relay endpoints rather than addresses, and nothing
// else changes. The server's word that a player left is that player's goodbye (bye_from), and a copy it removed ends its
// session with the server's reason. A guest on a server waits up to a minute for its host, whose place the server
// holds while it comes back.

#include "netplay_internal.h"
#include "udp.h"

#include "zstd.h"

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
#include <random>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace netplay;
using namespace netplay::detail;
using Clock = std::chrono::steady_clock;

namespace {
    constexpr uint32_t SESSION_MAGIC = 0x534E504D; // "MPNS"
    constexpr uint32_t SESSION_VERSION = 9;
    constexpr uint8_t KIND_HELLO = 0;
    constexpr uint8_t KIND_INPUTS = 1; // a guest's to the host
    constexpr uint8_t KIND_FRAMES = 2; // the host's to a guest
    constexpr uint8_t KIND_BYE = 3;    // a copy leaving the lobby
    constexpr uint8_t KIND_DESYNC = 4; // a desynced frame's page hashes, to name what differs
    constexpr uint32_t PAGES_SENT = 160; // page hashes a desync packet carries
    constexpr uint8_t KIND_RESYNC = 5;      // the host's to a guest: the frame the copies resync after, and the save
    constexpr uint8_t KIND_RESYNC_ACK = 6;  // a guest's to the host: it has the save, or the pieces it still wants
    constexpr uint8_t KIND_SAVE_PIECE = 7;  // the host's to a guest: a piece of the save, compressed
    constexpr uint32_t PIECE_SIZE = 1200;
    constexpr uint32_t PIECES_WANTED = 96;  // pieces a guest asks for at once
    constexpr int MAX_RESYNCS = 8;          // a session that keeps desyncing ends after this many
    constexpr uint8_t LEFT_RESYNC = 0x80;   // in a whole frame's left byte: the copies resync after this frame
    constexpr uint8_t KIND_NOTICE = 8;      // the host's to the guests: a player left (the host chooses), or back to the lobby
    constexpr uint8_t NOTICE_NONE = 0, NOTICE_LEFT = 1, NOTICE_LOBBY = 2;
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
    int delay = 3;             // this copy's own delay now: how far ahead of the frame it plays it samples its pad
    int first_delay = 3;       // the delay the session started with: the first frames, with no one's inputs
    int target_delay = 3;      // the delay the host wants everyone at (in its whole frames' packets)
    bool delay_forced = false; // MP_NETPLAY_DELAY, on the host: no measuring
    bool delay_known = false;  // the host has chosen the session's delay (a guest: and sent it)
    int64_t session_frames = -1;
    int timeout_ms = 15000; // the host, gone quiet this long, ends a guest's session
    int drop_ms = 5000;     // a guest, gone quiet this long, leaves the host's session
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
    size_t said_in_play = 0;     // where in said the lines of the session's play begin
    bool lobby_started = false;  // the host's player pressed Start: how many play is settled
    std::atomic<bool> lobby_start_wanted{ false };
    std::atomic<bool> lobby_cancel{ false };
    std::thread lobby_thread;
    std::mutex status_mutex;
    LobbyStatus status;          // under status_mutex
    size_t reason_from = 0;      // where in said the lines of the lobby's last step begin: why it failed, if it did
    int jitter_ms = 0; // MP_NETPLAY_JITTER_MS: a stall of up to this long before one frame in four, for the tests

    double recent_wait_ms = 0;          // this copy's frames' waits in the last second (note_wait)
    std::vector<Inputs> local_inputs;   // by frame
    std::vector<uint8_t> local_vis;     // on the host: each frame's VIs (Frame's vis), by frame, beside its pad
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
        uint64_t last_echo = 0, rtt_at = 0;
        std::vector<double> rtts_ms; // the latest: at most 32 from the hellos, 64 in play
        double waited_ms = 0;        // a guest's frames' waits in its last second, as it last said
        // The lobby: whether it has readied up, and its player's name.
        bool ready = false;
        std::string name;
        // A party the host resumes: whether this guest's copy has its save (0 not yet said, 1 yes, 2 no).
        uint8_t has_party = 0;
        // On the host: this guest has the save the copies resync with (KIND_RESYNC_ACK).
        bool resync_ready = false;
        // On the host, a guest that left during the session (said goodbye, or went quiet for drop_ms): the first
        // frame its inputs are empty from, where a computer player takes its place (Frame's left). UINT32_MAX: still in.
        uint32_t left_from = UINT32_MAX;
        // On the host, one that left and has come back: the frame it plays from again (its inputs empty and the
        // computer playing it before), UINT32_MAX while it hasn't; and one whose copy is joining again, not yet back.
        uint32_t left_until = UINT32_MAX;
        bool rejoining = false;
    };
    // On the host, whether a guest's inputs make the frames: it hasn't left, or has come back.
    bool playing(const Remote& r) {
        return r.left_from == UINT32_MAX || r.left_until != UINT32_MAX;
    }
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
    int rejoin_slot(const udp::Endpoint& from);
    bool receive_packet(int wait_ms);
    void drop_guest(int p);
    void leave_session(int p, const char* how);
    void guest_gone(int p, const char* how);
    void check_left_choice();

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
    std::vector<uint8_t> wholes_vis; // on a guest: each whole frame's VIs, the host's
    std::vector<uint8_t> wholes_left; // on a guest: each whole frame's players that have left

    struct Writer {
        uint8_t buf[1472]; // a UDP datagram that fits a 1500-byte link
        size_t n = 0;
        void u8(uint8_t v) { if (n < sizeof(buf)) buf[n++] = v; }
        void u16(uint16_t v) { u8(uint8_t(v)); u8(uint8_t(v >> 8)); }
        void u32(uint32_t v) { for (int i = 0; i < 4; i++) u8(uint8_t(v >> (8 * i))); }
        void u64(uint64_t v) { u32(uint32_t(v)); u32(uint32_t(v >> 32)); }
        void pad(const Pad& p) { u16(p.button); u8(uint8_t(p.stick_x)); u8(uint8_t(p.stick_y)); u8(p.err_no); }
        void bytes(const uint8_t* p, size_t size) { for (size_t i = 0; i < size; i++) u8(p[i]); }
    };
    struct Reader;
    void take_notice(Reader& r);
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

    // A resync (start_resync): after a desync the host has every copy take up its last party save after the same frame,
    // resync_at, and holds that frame back until each guest has the save: from its own party.snap when it is the
    // same, or else in pieces from the host's, compressed (KIND_SAVE_PIECE).
    bool resyncing = false;
    uint32_t resync_at = UINT32_MAX;
    uint32_t last_resync_at = UINT32_MAX; // the frame of the last resync: a desync before it is over
    PartySave resync_head;
    std::vector<uint8_t> resync_bytes;   // the save, as party.snap's bytes
    std::vector<uint8_t> resync_packed;  // compressed: the host's to send, a guest's as its pieces come
    uint32_t resync_raw_size = 0;
    std::vector<uint8_t> piece_have;     // a guest's: the pieces that have come
    bool resync_ready = false;           // a guest's: it has the save
    int resyncs = 0;
    uint8_t resync_back = 0;             // the ports a rejoin gives back to their players (Game's give_back)
    bool joining_now = false;            // a guest's: it joins a session in progress, and hasn't the save yet
    // On the host, players who left during play while its player chooses (Game's ask_left): the game waits for them,
    // as for any player's inputs, and the guests are told why (KIND_NOTICE).
    uint8_t gone_mask = 0;
    uint8_t gone_bye = 0; // of those, the ones that said goodbye (closed): the others may come back
    std::atomic<LeftChoice> left_choice{ LeftChoice::None };

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
        if (code != 0 && game.back_to_menu != nullptr) {
            // The reason: the last line the session said in play, without its frame.
            std::string text = said.substr(std::min(said_in_play, said.size()));
            while (!text.empty() && text.back() == '\n') text.pop_back();
            size_t nl = text.rfind('\n');
            if (nl != std::string::npos) text = text.substr(nl + 1);
            if (text.rfind("frame ", 0) == 0 && text.find(": ") != std::string::npos) text = text.substr(text.find(": ") + 2);
            if (!text.empty()) text[0] = char(toupper(uint8_t(text[0])));
            udp::close();
            game.back_to_menu(code, text.c_str(), nullptr);
        }
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

    // MP_NETPLAY_LATENCY_MS=<ms>[:<jitter ms>[:<from s>:<for s>:<extra ms>]], for the tests: each packet this copy sends
    // leaves that much later, give or take the jitter, with the extra added for a while from that many seconds in.
    int latency_ms = 0, latency_jitter_ms = 0, spike_from_s = 0, spike_for_s = 0, spike_ms = 0;
    Clock::time_point latency_start = Clock::now();
    struct Delayed {
        Clock::time_point at;
        udp::Endpoint to;
        std::vector<uint8_t> bytes;
    };
    std::deque<Delayed> delayed; // under delayed_mutex, sent by a thread of its own when each is due
    std::mutex delayed_mutex;
    std::condition_variable delayed_cv;
    bool delayed_started = false;

    // Once the session plays, a thread of its own takes in each packet as it comes (receive_thread), so that a whole
    // frame goes on from the host, and reaches a guest's frame, without waiting for either's next frame. The session's
    // state is under session_mutex; a frame that waits for its inputs waits on session_cv, which each packet wakes.
    std::recursive_mutex session_mutex;
    std::condition_variable_any session_cv;
    uint32_t frames_sent_to = 0; // on the host: how far the whole frames it has sent at a packet's coming go
    bool receiving = false;

    // The session's own count of frames, which its inputs, whole frames and hashes go by. It is the game's frame
    // (netplay::frame) until a copy takes up a snapshot of a party it resumes, which puts the game at the snapshot's
    // frame: every copy does so at the same session frame, and the session goes on counting as before.
    uint64_t sframe = 0;

    // Resuming a party (SessionSettings' resume): the host offers the party its copy saved last, and each guest's copy
    // says whether it has the same one (party_save, netplay_internal.h). Once every copy has it, each takes it up at
    // the same session frame (netplay_core.cpp). A copy without it ends the session, saying so: it can't play that
    // party.
    bool resuming = false;     // the host's, from its settings; a guest's, from the host's hellos
    PartySave offered;         // the party the host offers
    uint8_t has_party = 0;     // a guest's answer (Remote's has_party)
    bool party_taken = false;  // this copy has asked for its save to be taken up

    void send_delayed() {
        std::unique_lock lock{ delayed_mutex };
        while (true) {
            auto now = Clock::now();
            auto next = Clock::time_point::max();
            for (auto it = delayed.begin(); it != delayed.end();) {
                if (it->at <= now) {
                    udp::send(it->to, it->bytes.data(), it->bytes.size());
                    it = delayed.erase(it);
                } else {
                    next = std::min(next, it->at);
                    ++it;
                }
            }
            if (next == Clock::time_point::max()) {
                delayed_cv.wait(lock);
            } else {
                delayed_cv.wait_until(lock, next);
            }
        }
    }

    void send(const Remote& to, const Writer& w) {
        if (!to.known || (drop_percent > 0 && rand() % 100 < drop_percent)) {
            return;
        }
        if (latency_ms > 0 || latency_jitter_ms > 0 || spike_ms > 0) {
            double s_in = std::chrono::duration<double>(Clock::now() - latency_start).count();
            int ms = latency_ms + (latency_jitter_ms > 0 ? rand() % (latency_jitter_ms + 1) : 0);
            if (spike_ms > 0 && s_in >= spike_from_s && s_in < spike_from_s + spike_for_s) {
                ms += spike_ms;
            }
            std::lock_guard lock{ delayed_mutex };
            if (!delayed_started) {
                delayed_started = true;
                std::thread(send_delayed).detach();
            }
            delayed.push_back({ Clock::now() + std::chrono::milliseconds(ms), to.endpoint,
                    std::vector<uint8_t>(w.buf, w.buf + w.n) });
            delayed_cv.notify_one();
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
        // The party: the host's id for it, and whether it resumes one (which: its frame and hash); a guest's answer.
        w.u64(party_id);
        w.u8(resuming ? 1 : 0);
        w.u64(offered.frame);
        w.u64(offered.hash);
        w.u8(has_party);
        // A guest's player in the last party it saved, and that party's id, for the host to give it the same player
        // when it resumes that party.
        PartySave mine;
        bool saved = !hosting && party_resumable(mine);
        w.u8(uint8_t(saved ? mine.player : 0xFF));
        w.u64(saved ? mine.id : 0);
        // The host's to a guest joining again: the session is in progress.
        w.u8(hosting && handshaken ? 1 : 0);
        send(to, w);
    }

    // Round trips during play too: each inputs and frames packet carries the clocks a hello does, and a guest's says how
    // much its frames waited lately, for the host's choice of delay (adjust_delay).
    void put_clock(Writer& w, const Remote& to) {
        w.u64(clock_us());
        w.u64(to.their_clock);
        w.u32(to.their_clock != 0 ? uint32_t(clock_us() - to.their_clock_at) : 0);
    }

    void take_clock(Reader& r, Remote& from) {
        uint64_t their_clock = r.u64();
        uint64_t echo = r.u64();
        uint32_t held = r.u32();
        if (!r.ok) {
            return;
        }
        from.their_clock = their_clock;
        from.their_clock_at = clock_us();
        // One a 100 ms is plenty, and keeps the latest 64 to about 6 seconds' worth.
        if (echo != 0 && echo != from.last_echo && clock_us() - echo > held && clock_us() - from.rtt_at > 100000) {
            from.last_echo = echo;
            from.rtt_at = clock_us();
            from.rtts_ms.push_back(double(clock_us() - echo - held) / 1000.0);
            if (from.rtts_ms.size() > 64) {
                from.rtts_ms.erase(from.rtts_ms.begin());
            }
        }
    }

    // The first frame some player's input is still missing for: on the host, how far the whole frames go.
    uint32_t wholes_ready() {
        uint32_t ready = uint32_t(local_inputs.size());
        for (int p = 1; p < players; p++) {
            if (playing(remotes[p])) {
                ready = std::min(ready, remotes[p].next);
                if (resyncing && !remotes[p].resync_ready) {
                    ready = std::min(ready, resync_at); // the frame the copies resync after waits for its save
                }
            }
        }
        return ready;
    }

    Inputs input_of(int p, uint32_t f) {
        if (p == 0) {
            return local_inputs[f];
        }
        const Remote& r = remotes[p];
        return f < r.left_from || f >= r.left_until ? r.inputs[f] : Inputs{};
    }

    // On the host, the players that have left by frame f (Frame's left).
    uint8_t left_at(uint32_t f) {
        uint8_t mask = 0;
        for (int p = 1; p < players; p++) {
            if (f >= remotes[p].left_from && f < remotes[p].left_until) {
                mask |= uint8_t(1u << p);
            }
        }
        return mask;
    }

    // A guest's to the host.
    void send_inputs() {
        Remote& host = remotes[0];
        Writer w;
        header(w, KIND_INPUTS);
        w.u32(host.next);
        uint32_t first = std::max<uint32_t>(host.ack, uint32_t(first_delay));
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
        put_clock(w, host);
        w.u16(uint16_t(std::min(65535.0, recent_wait_ms)));
        send(host, w);
    }

    // The host's to each guest.
    void send_frames() {
        uint32_t ready = wholes_ready();
        for (int p = 1; p < players; p++) {
            Remote& guest = remotes[p];
            if (!playing(guest)) {
                continue;
            }
            Writer w;
            header(w, KIND_FRAMES);
            w.u32(guest.next);
            uint32_t first = std::max<uint32_t>(guest.ack, uint32_t(first_delay));
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
                w.u8(local_vis[f]);
                w.u8(uint8_t(left_at(f) | (resyncing && f == resync_at ? LEFT_RESYNC : 0)));
            }
            put_hashes(w);
            put_clock(w, guest);
            w.u8(uint8_t(target_delay));
            send(guest, w);
            if (resyncing && !guest.resync_ready) {
                Writer rw;
                header(rw, KIND_RESYNC);
                rw.u32(resync_at);
                rw.u64(resync_head.frame);
                rw.u64(resync_head.hash);
                rw.u64(resync_head.id);
                rw.u32(resync_raw_size);
                rw.u32(uint32_t(resync_packed.size()));
                rw.u8(resync_back);
                send(guest, rw);
            }
        }
    }

    uint32_t pieces_of(size_t size) {
        return uint32_t((size + PIECE_SIZE - 1) / PIECE_SIZE);
    }

    // A guest's answer to the host's KIND_RESYNC: it has the save, or the pieces it still wants, from the first missing.
    void send_resync_ack() {
        Writer w;
        header(w, KIND_RESYNC_ACK);
        w.u32(resync_at);
        w.u8(resync_ready ? 1 : 0);
        uint32_t first = 0;
        while (first < piece_have.size() && piece_have[first]) {
            first++;
        }
        uint32_t count = 0;
        std::vector<uint32_t> wanted;
        for (uint32_t i = first; i < piece_have.size() && count < PIECES_WANTED; i++) {
            if (!piece_have[i]) {
                wanted.push_back(i);
                count++;
            }
        }
        w.u8(uint8_t(std::min<size_t>(wanted.size(), 255)));
        for (size_t i = 0; i < wanted.size() && i < 255; i++) {
            w.u32(wanted[i]);
        }
        send(remotes[0], w);
    }

    void send_all() {
        if (hosting) {
            send_frames();
        } else {
            send_inputs();
            if (resyncing) {
                send_resync_ack();
            }
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
        if (hosting && handshaken && from.rejoining) {
            from.rejoining = false; // a copy joining again that can't: the party goes on without it
            return;
        }
        if (hosting && in_lobby) {
            drop_guest(who); // the lobby stays open for the others
            return;
        }
        end_session(4);
    }

    // A desync: which 4 KB pages of RDRAM differ, from the page hashes the two copies swap (KIND_DESYNC), so the
    // session's log names where the games went apart, and RDRAM as it is now, to diff with the other copy's.
    bool desyncing = false;
    bool resync_failed = false;
    bool desync_logged = false;
    uint32_t desync_frame = 0;
    std::vector<uint64_t> their_pages;
    std::vector<uint8_t> their_have;

    void send_pages(const Remote& to, uint32_t f) {
        const uint64_t* pages = page_hashes(f);
        if (pages == nullptr) {
            return;
        }
        for (uint32_t first = 0; first < PAGES; first += PAGES_SENT) {
            Writer w;
            header(w, KIND_DESYNC);
            w.u32(f);
            w.u16(uint16_t(first));
            uint32_t count = std::min<uint32_t>(PAGES_SENT, uint32_t(PAGES) - first);
            w.u16(uint16_t(count));
            for (uint32_t i = 0; i < count; i++) {
                w.u64(pages[first + i]);
            }
            send(to, w);
        }
    }

    void take_pages(Reader& r) {
        uint32_t f = r.u32();
        uint32_t first = r.u16();
        uint32_t count = r.u16();
        if (!r.ok || f != desync_frame || first + count > PAGES) {
            return;
        }
        for (uint32_t i = 0; i < count; i++) {
            uint64_t h = r.u64();
            if (!r.ok) {
                return;
            }
            their_pages[first + i] = h;
            their_have[first + i] = 1;
        }
    }

    void log_differing_pages(int who) {
        const uint64_t* ours = page_hashes(desync_frame);
        if (ours == nullptr || std::find(their_have.begin(), their_have.end(), 0) != their_have.end()) {
            log_session_line("[netplay] the pages that differ: unknown (the other copy's didn't all come)\n");
            return;
        }
        std::string line = "[netplay] RDRAM that differs from player " + std::to_string(who + 1) + "'s:";
        size_t ranges = 0;
        for (size_t p = 0; p < PAGES; p++) {
            if (ours[p] == their_pages[p]) {
                continue;
            }
            size_t end = p;
            while (end + 1 < PAGES && ours[end + 1] != their_pages[end + 1]) {
                end++;
            }
            char range[40];
            snprintf(range, sizeof(range), " %08X-%08X", uint32_t(0x80000000 + p * PAGE_SIZE),
                    uint32_t(0x80000000 + (end + 1) * PAGE_SIZE - 1));
            line += range;
            ranges++;
            p = end;
        }
        line += ranges == 0 ? " none in the pages (the frame's other words)\n" : "\n";
        fputs(line.c_str(), stderr);
        log_session_line(line.c_str());
    }

    // Both copies play on to the same frame after the desync, DESYNC_PLAY frames on, and write RDRAM there, so the two
    // files differ only where the games do; meanwhile they swap the desynced frame's page hashes.
    constexpr uint32_t DESYNC_PLAY = 60;
    int desync_who = 0;

    void desynced(uint32_t f, int who) {
        if (!desyncing || desync_frame != f) {
            desyncing = true;
            desync_frame = f;
            their_pages.assign(PAGES, 0);
            their_have.assign(PAGES, 0);
        }
        desync_who = who;
        say("[netplay] frame %u differs from player %d's: the session has desynced\n", f, who + 1);
    }

    // The resync, on the host, once a desync is found: every copy takes up the host's last party save after resync_at,
    // which waits for each guest to have it. Without a save to go back to, the session ends as it did before.
    // On the host: its party save, read and compressed to send.
    bool pack_save() {
        if (!party_bytes(resync_bytes) || !snapshot_head(resync_bytes, resync_head)) {
            return false;
        }
        resync_packed.resize(ZSTD_compressBound(resync_bytes.size()));
        size_t packed = ZSTD_compress(resync_packed.data(), resync_packed.size(), resync_bytes.data(),
                resync_bytes.size(), 1);
        if (ZSTD_isError(packed)) {
            return false;
        }
        resync_packed.resize(packed);
        resync_raw_size = uint32_t(resync_bytes.size());
        return true;
    }

    bool start_resync() {
        if (resyncs >= MAX_RESYNCS || !pack_save()) {
            return false;
        }
        resync_back = 0;
        // After the frame both copies write RDRAM at for the record, and after every frame already sent.
        resync_at = std::max<uint32_t>(std::max<uint32_t>(frames_sent_to, wholes_ready()), desync_frame + DESYNC_PLAY + 1);
        resyncing = true;
        resyncs++;
        for (int p = 1; p < players; p++) {
            remotes[p].resync_ready = false;
        }
        say("[netplay] resyncing: every copy goes back to the host's save of frame %" PRIu64 " after frame %u (%zu KB "
            "to send a copy without it)\n", resync_head.frame, resync_at, resync_packed.size() / 1024);
        if (game.show_resync != nullptr) {
            game.show_resync(true);
        }
        return true;
    }

    // On a guest, the host's KIND_RESYNC: the save is this copy's own party.snap when it is the same, or else comes in
    // pieces.
    void take_resync(Reader& r) {
        uint32_t at = r.u32();
        PartySave head;
        head.frame = r.u64();
        head.hash = r.u64();
        head.id = r.u64();
        uint32_t raw = r.u32();
        uint32_t packed = r.u32();
        uint8_t back = r.u8();
        if (!r.ok || (resyncing && at == resync_at)) {
            return;
        }
        resync_back = back;
        resyncing = true;
        resync_at = at;
        resync_head = head;
        resync_raw_size = raw;
        PartySave mine;
        // MP_NETPLAY_TEST_SEND_SAVE, for the tests: the host sends its save whatever this copy has.
        resync_ready = getenv("MP_NETPLAY_TEST_SEND_SAVE") == nullptr && party_bytes(resync_bytes) &&
            snapshot_head(resync_bytes, mine) && mine.frame == head.frame && mine.hash == head.hash && mine.id == head.id;
        if (!resync_ready) {
            resync_bytes.clear();
            resync_packed.assign(packed, 0);
            piece_have.assign(pieces_of(packed), 0);
        } else {
            resync_packed.clear();
            piece_have.clear();
        }
        say("[netplay] resyncing: going back to the host's save of frame %" PRIu64 " after frame %u (%s)\n", head.frame,
            at, resync_ready ? "this copy has it" : "the host sends it");
        if (game.show_resync != nullptr) {
            game.show_resync(true);
        }
        send_resync_ack();
    }

    // On a guest, a piece of the save; once all have come, the save.
    void take_piece(Reader& r) {
        uint32_t at = r.u32();
        uint32_t index = r.u32();
        uint32_t size = r.u16();
        if (!r.ok || !resyncing || resync_ready || at != resync_at || index >= piece_have.size() ||
                size_t(index) * PIECE_SIZE + size > resync_packed.size() || r.at + size > r.n) {
            return;
        }
        memcpy(resync_packed.data() + size_t(index) * PIECE_SIZE, r.buf + r.at, size);
        piece_have[index] = 1;
        if (std::find(piece_have.begin(), piece_have.end(), 0) != piece_have.end()) {
            // Asks for more once the pieces asked for last have come, rather than at the next frame.
            if (index + 1 == piece_have.size() || (index + 1) % PIECES_WANTED == 0) {
                send_resync_ack();
            }
            return;
        }
        resync_bytes.resize(resync_raw_size);
        size_t got = ZSTD_decompress(resync_bytes.data(), resync_bytes.size(), resync_packed.data(), resync_packed.size());
        PartySave head;
        if (ZSTD_isError(got) || got != resync_raw_size || !snapshot_head(resync_bytes, head) ||
                head.hash != resync_head.hash || head.frame != resync_head.frame) {
            say("[netplay] the host's save came damaged: asking again\n");
            piece_have.assign(piece_have.size(), 0);
            resync_bytes.clear();
            return;
        }
        resync_packed.clear();
        resync_ready = true;
        say("[netplay] has the host's save\n");
        send_resync_ack();
    }

    // On the host, a guest's KIND_RESYNC_ACK: it has the save, or the pieces it wants next.
    void take_resync_ack(Reader& r, Remote& guest) {
        uint32_t at = r.u32();
        uint8_t ready = r.u8();
        uint32_t count = r.u8();
        if (!r.ok || !resyncing || at != resync_at) {
            return;
        }
        if (ready) {
            guest.resync_ready = true;
            return;
        }
        for (uint32_t i = 0; i < count; i++) {
            uint32_t index = r.u32();
            if (!r.ok || index >= pieces_of(resync_packed.size())) {
                return;
            }
            Writer w;
            header(w, KIND_SAVE_PIECE);
            w.u32(resync_at);
            w.u32(index);
            size_t from = size_t(index) * PIECE_SIZE;
            uint32_t size = uint32_t(std::min<size_t>(PIECE_SIZE, resync_packed.size() - from));
            w.u16(uint16_t(size));
            w.bytes(resync_packed.data() + from, size);
            send(guest, w);
        }
    }

    // After the frame the copies resync after: from the next frame, the hashes compare again.
    void finish_resync(bool take = true) {
        last_resync_at = resync_at;
        if (take) {
            resync_with(std::move(resync_bytes), resync_back);
        }
        resync_back = 0;
        resync_bytes.clear();
        resync_packed.clear();
        piece_have.clear();
        resyncing = false;
        resync_ready = false;
        desyncing = false;
        desync_logged = false;
        for (Remote& r : remotes) {
            r.resync_ready = false;
            r.compare_from = std::max<uint32_t>(r.compare_from, resync_at + 1);
            while (!r.hashes.empty() && r.hashes.front().first <= resync_at) {
                r.hashes.pop_front();
            }
        }
        if (game.show_resync != nullptr) {
            game.show_resync(false);
        }
    }

    // Each frame after a desync: the pages to the other copy, and at the frame both stop at, RDRAM for the record and
    // the resync (the host starts it; a guest waits for the host's).
    void desync_frame_step() {
        if (!desyncing) {
            return;
        }
        if (hosting && !resyncing && !start_resync()) {
            resync_failed = true;
        }
        Remote& other = remotes[hosting ? desync_who : 0];
        if (sframe % 4 == 0) {
            send_pages(other, desync_frame);
        }
        if (desync_logged || sframe < uint64_t(desync_frame) + DESYNC_PLAY) {
            return;
        }
        desync_logged = true;
        dump_rdram_now("desync-frame" + std::to_string(sframe));
        auto until = Clock::now() + std::chrono::milliseconds(1000);
        while (Clock::now() < until && std::find(their_have.begin(), their_have.end(), 0) != their_have.end()) {
            send_pages(other, desync_frame);
            receive_packet(16);
        }
        for (int i = 0; i < 5; i++) {
            send_all();
            send_pages(other, desync_frame); // the other may still be waiting for this copy's
        }
        log_differing_pages(desync_who);
        if (resync_failed) {
            say("[netplay] the session can't go back to a save of the party: it has ended\n");
            end_session(2);
        }
    }

    void compare_hashes(Remote& from, int who) {
        while (!desyncing && !from.hashes.empty() && from.hashes.front().first < local_hashes.size()) {
            auto [f, h] = from.hashes.front();
            from.hashes.pop_front();
            if (local_hashes[f] != h) {
                desynced(f, who);
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
            if (!guest.have[f]) { // a frame the host has made already stays as it was made (a rejoin's empty ones)
                guest.inputs[f] = in;
                guest.have[f] = 1;
            }
        }
        while (guest.next < guest.have.size() && guest.have[guest.next]) {
            guest.next++;
        }
        take_hashes(r, guest, who);
        take_clock(r, guest);
        uint16_t waited = r.u16();
        if (r.ok) {
            guest.waited_ms = waited;
        }
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
            uint8_t vis = r.u8();
            uint8_t left = r.u8();
            if (!r.ok) {
                return;
            }
            if (wholes.size() <= f) {
                wholes.resize(f + 1);
                whole_have.resize(f + 1, 0);
                wholes_vis.resize(f + 1, 2);
                wholes_left.resize(f + 1, 0);
            }
            wholes[f] = whole;
            whole_have[f] = 1;
            wholes_vis[f] = uint8_t(std::clamp<int>(vis, 2, 4));
            wholes_left[f] = left;
        }
        while (host.next < whole_have.size() && whole_have[host.next]) {
            host.next++;
        }
        take_hashes(r, host, 0);
        take_clock(r, host);
        int target = r.u8();
        if (r.ok && target >= 1 && target <= 15) {
            target_delay = target;
        }
    }

    // A copy's goodbye (KIND_BYE), or a netplay server's word that it left (udp::relay_take_gone).
    void bye_from(const udp::Endpoint& from_endpoint) {
        if (hosting) {
            for (int p = 1; p < players; p++) {
                if (remotes[p].known && remotes[p].endpoint == from_endpoint) {
                    if (handshaken) {
                        guest_gone(p, "left");
                    } else {
                        say("[netplay] player %d left\n", p + 1);
                        drop_guest(p);
                    }
                    break;
                }
            }
            return;
        }
        if (from_endpoint == remotes[0].endpoint && !handshaken) {
            say("[netplay] the host closed the lobby\n");
            end_session(3);
        }
        if (from_endpoint == remotes[0].endpoint && handshaken && !in_lobby) {
            say("[netplay] the host left the party. The party was saved at the start of the last scene: the host can "
                "pick it up with Resume Party.\n");
            end_session(3);
        }
    }

    // Takes in one packet, if one came within wait_ms. Returns whether one did.
    bool handle_packet(const uint8_t* buf, size_t n, const udp::Endpoint& from_endpoint);

    // A packet, if one comes within wait_ms, taken in under the session's lock. On the host, the whole frames it
    // completes go to the guests at once, rather than at the host's next frame.
    bool receive_packet(int wait_ms) {
        uint8_t buf[1500];
        udp::Endpoint from_endpoint;
        size_t n = udp::receive(buf, sizeof(buf), wait_ms, from_endpoint);
        if (udp::relay_on()) {
            // Through a netplay server: a player it says has left left, and this copy removed from it is out of the
            // session.
            std::lock_guard<std::recursive_mutex> lock(session_mutex);
            udp::Endpoint gone;
            while (udp::relay_take_gone(gone)) {
                bye_from(gone);
            }
            std::string why = udp::relay_ended();
            if (!why.empty()) {
                while (!why.empty() && (why.back() == '.' || why.back() == '\n')) why.pop_back();
                if (!why.empty()) why[0] = char(tolower(uint8_t(why[0])));
                say("[netplay] %s\n", why.c_str());
                end_session(3);
            }
        }
        if (n == 0) {
            return false;
        }
        std::lock_guard<std::recursive_mutex> lock(session_mutex);
        bool taken = handle_packet(buf, n, from_endpoint);
        if (taken && handshaken && hosting && wholes_ready() > frames_sent_to) {
            frames_sent_to = wholes_ready();
            send_frames();
        }
        session_cv.notify_all();
        return taken;
    }

    bool handle_packet(const uint8_t* buf, size_t n, const udp::Endpoint& from_endpoint) {
        Reader r{ buf, n };
        if (r.u32() != SESSION_MAGIC || r.u32() != SESSION_VERSION) {
            return false;
        }
        uint8_t kind = r.u8();
        int who = r.u8();
        if (r.ok && kind == KIND_BYE) {
            bye_from(from_endpoint);
            return true;
        }
        if (hosting && r.ok && kind == KIND_HELLO && (who == UNASSIGNED || !handshaken)) {
            // Before the session starts a guest is the player its address has (the guests move up when one leaves);
            // during it, one joining again takes the place of a player who left.
            who = handshaken ? rejoin_slot(from_endpoint) : slot_for(from_endpoint);
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
            uint64_t their_party = r.u64();
            bool their_resuming = r.u8() != 0;
            PartySave their_offer;
            their_offer.frame = r.u64();
            their_offer.hash = r.u64();
            uint8_t their_has_party = r.u8();
            uint8_t their_saved_player = r.u8();
            uint64_t their_saved_party = r.u64();
            bool in_progress = r.u8() != 0;
            if (!r.ok) {
                return true;
            }
            if (!hosting && !handshaken && in_progress && !joining_now) {
                // Joining a session in progress, as a player who left it: the game waits for the host's save.
                joining_now = true;
                say("[netplay] the party is in progress: joining it again\n");
            }
            if (joining_now) {
                their_resuming = false;
            }
            if (hosting && resuming && !handshaken && their_saved_party == party_id && their_saved_player >= 1 &&
                    their_saved_player < PORTS && their_saved_player != who && !remotes[their_saved_player].known) {
                // A guest of the party resumed plays the player it played, whichever order the guests join in: it
                // moves there, and the host's hellos tell it.
                std::swap(remotes[who], remotes[their_saved_player]);
                who = their_saved_player;
            }
            Remote& from = remotes[hosting ? who : 0];
            if (hosting) {
                from.has_party = their_has_party;
            } else if (!handshaken) {
                party_id = their_party;
                resuming = their_resuming;
                offered.frame = their_offer.frame;
                offered.hash = their_offer.hash;
                if (resuming && has_party == 0) {
                    PartySave mine;
                    bool same = party_resumable(mine) && mine.id == their_party && mine.frame == their_offer.frame &&
                        mine.hash == their_offer.hash;
                    has_party = same ? 1 : 2;
                    if (!same) {
                        say("[netplay] the host resumes a party this copy hasn't the save of: only the players of that party "
                            "can resume it\n");
                        for (int i = 0; i < 10; i++) {
                            send_hello(from);
                        }
                        end_session(4);
                    }
                }
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
            if (handshaken && (!saw || from.rejoining)) {
                send_hello(from); // it is still waiting for this copy's identity, or to be let in again
            }
        } else if (kind == KIND_INPUTS && hosting) {
            if (from.identity_ok) {
                from.saw_ours = true;
            }
            if (handshaken && playing(from)) { // its buffers are made once the handshake ends
                take_inputs(r, from, who);
            }
        } else if (kind == KIND_NOTICE && handshaken && !hosting) {
            take_notice(r);
        } else if (kind == KIND_RESYNC && handshaken && !hosting) {
            take_resync(r);
        } else if (kind == KIND_SAVE_PIECE && handshaken && !hosting) {
            take_piece(r);
        } else if (kind == KIND_RESYNC_ACK && handshaken && hosting) {
            take_resync_ack(r, from);
        } else if (kind == KIND_DESYNC && handshaken) {
            if (!desyncing) {
                // The other copy found the frame first: this one names it too, with the frame the other names.
                Reader peek = r;
                uint32_t f = peek.u32();
                if (peek.ok && f < local_hashes.size() && (last_resync_at == UINT32_MAX || f > last_resync_at)) {
                    desyncing = true;
                    desync_frame = f;
                    their_pages.assign(PAGES, 0);
                    their_have.assign(PAGES, 0);
                    take_pages(r);
                    desynced(f, who);
                    return true;
                }
            } else {
                take_pages(r);
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
            if (resuming && remotes[p].has_party == 2) {
                say("[netplay] player %d's copy hasn't this party's save: only the players of that party can resume it\n", p + 1);
                end_session(4);
            }
            if (resuming && remotes[p].has_party != 1) {
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

    // On the host, during the session: a copy saying hello is a player who left joining again, in the place of the
    // first who has left (a computer plays it, or the game waits for it), unless it is one still playing.
    int rejoin_slot(const udp::Endpoint& from) {
        for (int p = 1; p < players; p++) {
            if (remotes[p].known && remotes[p].endpoint == from && (playing(remotes[p]) || remotes[p].rejoining) &&
                    !(gone_mask & (1u << p))) {
                return p;
            }
        }
        for (int p = 1; p < players; p++) {
            Remote& r = remotes[p];
            if (r.rejoining && r.endpoint == from) {
                return p;
            }
        }
        for (int p = 1; p < players; p++) {
            Remote& r = remotes[p];
            if (!r.rejoining && (!playing(r) || (gone_mask & (1u << p)))) {
                r.rejoining = true;
                r.identity_ok = false;
                r.saw_ours = false;
                r.endpoint = from;
                say("[netplay] frame %" PRIu64 ": player %d is joining again, from %s\n", sframe, p + 1,
                        from.text().c_str());
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
    std::deque<std::pair<Clock::time_point, double>> wait_window; // the last second's waits
    void note_wait(double ms) {
        auto now = Clock::now();
        if (ms > 1.0 && ms < 1000.0) { // a longer one waited for a player who left, not for the connection
            wait_window.emplace_back(now, ms);
        }
        while (!wait_window.empty() && now - wait_window.front().first > std::chrono::seconds(1)) {
            wait_window.pop_front();
        }
        recent_wait_ms = 0;
        for (const auto& w : wait_window) {
            recent_wait_ms += w.second;
        }
        if (ms > 1.0) {
            waited_ms += ms;
            longest_wait_ms = std::max(longest_wait_ms, ms);
            frames_waited++;
        }
        if (sframe % 300 == 299) {
            char line[200];
            snprintf(line, sizeof(line), "[netplay] frames to %" PRIu64 ": %d waited, %.0f ms in all, the longest %.0f ms; "
                    "delay %d\n", sframe, frames_waited, waited_ms, longest_wait_ms, delay);
            log_session_line(line);
            waited_ms = longest_wait_ms = 0;
            frames_waited = 0;
        }
    }

    // On the host, every half second of play: the delay everyone should be at. It covers the slowest guest's round trip
    // as it is most of the time (the 90th percentile of the last few seconds') with a little to spare, and goes up at
    // once when that grows or someone's frames wait a frame's time in a second, but down only a frame at a time, after
    // ten seconds in which it could have: a steady delay feels better than the lowest one (docs/netplay.md).
    Clock::time_point lower_since{};
    void adjust_delay() {
        if (!hosting || delay_forced || sframe % 15 != 0) {
            return;
        }
        double worst_rtt = 0, worst_wait = recent_wait_ms;
        for (int p = 1; p < players; p++) {
            std::vector<double> v = remotes[p].rtts_ms;
            if (v.size() >= 8) {
                std::sort(v.begin(), v.end());
                worst_rtt = std::max(worst_rtt, v[v.size() * 9 / 10]);
            }
            worst_wait = std::max(worst_wait, remotes[p].waited_ms);
        }
        constexpr double frame_ms = 1000.0 / 30.0;
        int need = std::clamp(int(std::ceil((worst_rtt + 20.0) / frame_ms)), 2, 15);
        int wanted = target_delay;
        if (need > target_delay) {
            wanted = need;
        } else if (worst_wait > frame_ms && target_delay < std::min(need + 2, 15)) {
            // Frames that wait though the delay covers the round trips: their inputs came late (jitter), which a frame
            // more covers, or a copy is slower than the others (a scene it loads, a computer that can't keep up),
            // which no delay helps; so no more than two frames past what the round trips need.
            wanted = target_delay + 1;
        }
        auto now = Clock::now();
        if (wanted > target_delay) {
            lower_since = now;
        } else if (need < target_delay && worst_wait < 1.0) {
            if (lower_since == Clock::time_point{}) {
                lower_since = now;
            } else if (now - lower_since > std::chrono::seconds(10)) {
                wanted = target_delay - 1;
                lower_since = now;
            }
        } else {
            lower_since = Clock::time_point{};
        }
        if (wanted != target_delay) {
            char line[200];
            snprintf(line, sizeof(line), "[netplay] frame %" PRIu64 ": delay %d to %d (round trip %.0f ms, waits %.0f ms "
                    "in the last second)\n", sframe, target_delay, wanted, worst_rtt, worst_wait);
            fputs(line, stderr);
            log_session_line(line);
            target_delay = wanted;
        }
    }

    // This copy's pad for the frames its delay is ahead, a frame's at a time: a delay that grows by a frame samples
    // the pad for two frames this once, one that shrinks by a frame samples none, as the frame it would has its sample
    // already. Each copy moves its own, so the copies needn't agree on when: the frames' inputs are whatever each
    // player's copy sent, the same everywhere.
    void push_local(const Inputs& mine, uint8_t vis) {
        if (target_delay > delay && delay < 15) {
            local_inputs.push_back(mine);
            local_vis.push_back(vis);
            delay++;
        } else if (target_delay < delay && delay > 1 && local_inputs.size() > sframe + 1) {
            delay--;
            return;
        }
        local_inputs.push_back(mine);
        local_vis.push_back(vis);
    }

    // The buffers, once the delay is known: the first frames have no inputs from anyone.
    void prepare_buffers() {
        // The ports with a controller, from power-on (the game's entry comes after the handshake): one for each player.
        connected_mask = (1u << players) - 1;
        first_delay = target_delay = delay;
        local_inputs.assign(size_t(delay), Inputs{});
        local_vis.assign(size_t(delay), 2);
        for (Remote& r : remotes) {
            r.inputs.assign(size_t(delay), Inputs{});
            r.have.assign(size_t(delay), 1);
            r.next = uint32_t(delay);
        }
        wholes.assign(size_t(delay), Whole{});
        whole_have.assign(size_t(delay), 1);
        wholes_vis.assign(size_t(delay), 2);
        wholes_left.assign(size_t(delay), 0);
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
        if (resuming && hosting && int(offered.players) != players) {
            say("[netplay] the party resumed was of %d players, this session of %d\n", int(offered.players), players);
            end_session(4);
        }
        party_player = player;
        PartySave mine;
        if (resuming && !hosting && party_resumable(mine) && int(mine.player) != player) {
            say("[netplay] in the party resumed this copy's player was player %d, and now it is player %d: join in the "
                "order of last time\n", int(mine.player) + 1, player + 1);
            end_session(4);
        }
        if (resuming && !party_taken) {
            party_taken = true;
            resume_party();
            say("[netplay] resuming the party saved at its frame %" PRIu64 "\n", offered.frame);
        }
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
        status.resuming = resuming;
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
                    udp::relay_final();
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
                    // Through a server, a guest may wait for a host who is coming back to it (the server holds its
                    // place 45 seconds).
                    bool relayed = udp::relay_on();
                    if (silent > std::chrono::seconds(heard ? 6 : relayed ? 60 : 20)) {
                        say(heard     ? "[netplay] the host stopped answering\n"
                            : relayed ? "[netplay] no host came to the server\n"
                                      : "[netplay] the host never answered: check the address, and that the host's "
                                        "port is open to you\n");
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
        return hosting ? wholes_ready() > sframe : remotes[0].next > sframe;
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

    // The goodbye of a copy that is closing, so the others needn't wait for it to go quiet: the host's ends the guests'
    // sessions, a guest's leaves the host's to play on without it.
    void goodbye() {
        std::lock_guard lock(session_mutex);
        if (!handshaken || in_lobby) {
            return;
        }
        for (int i = 0; i < 5; i++) {
            Writer w;
            header(w, KIND_BYE);
            if (hosting) {
                for (int p = 1; p < players; p++) {
                    if (playing(remotes[p])) {
                        send(remotes[p], w);
                    }
                }
            } else {
                send(remotes[0], w);
            }
        }
    }

    // On the host, a guest gone from the session: from the first frame of its inputs still missing, its inputs are
    // empty and a computer player takes its place (Frame's left), and the session plays on without waiting for it.
    void leave_session(int p, const char* how) {
        Remote& guest = remotes[p];
        if (!playing(guest)) {
            return;
        }
        guest.left_from = std::max(guest.next, uint32_t(first_delay));
        guest.left_until = UINT32_MAX;
        say("[netplay] frame %" PRIu64 ": player %d %s; a computer player takes their place from frame %u\n", sframe,
                p + 1, how, guest.left_from);
        session_cv.notify_all();
    }

    void send_notice(uint8_t code, int about) {
        for (int p = 1; p < players; p++) {
            if (remotes[p].known && playing(remotes[p]) && !(gone_mask & (1u << p))) {
                Writer w;
                header(w, KIND_NOTICE);
                w.u8(code);
                w.u8(uint8_t(about));
                send(remotes[p], w);
            }
        }
    }

    std::string player_name(int p) {
        const std::string& name = remotes[p].name;
        return name.empty() ? "Player " + std::to_string(p + 1) : name;
    }

    void guest_gone(int p, const char* how) {
        if (!playing(remotes[p]) || (gone_mask & (1u << p))) {
            return;
        }
        if (game.ask_left == nullptr) {
            leave_session(p, how);
            return;
        }
        say("[netplay] frame %" PRIu64 ": player %d %s; waiting for the host's choice\n", sframe, p + 1, how);
        bool first = gone_mask == 0;
        gone_mask |= uint8_t(1u << p);
        if (strcmp(how, "left") == 0) {
            gone_bye |= uint8_t(1u << p);
        }
        if (first) {
            left_choice = LeftChoice::None;
        }
        game.ask_left(p, player_name(p).c_str());
    }

    // On the host, each time the frame waits: the guests told, and the player's choice once it comes.
    void check_left_choice() {
        if (gone_mask == 0) {
            return;
        }
        int about = 0;
        for (int p = 1; p < players; p++) {
            if (gone_mask & (1u << p)) about = p;
        }
        LeftChoice choice = left_choice.load();
        if (choice == LeftChoice::None) {
            // One that only went quiet and is heard again (its connection came back) plays on.
            for (int p = 1; p < players; p++) {
                if ((gone_mask & ~gone_bye & (1u << p)) && Clock::now() - remotes[p].heard < std::chrono::seconds(1)) {
                    gone_mask &= uint8_t(~(1u << p));
                    say("[netplay] frame %" PRIu64 ": player %d is back\n", sframe, p + 1);
                }
            }
            if (gone_mask == 0) {
                game.ask_left(-1, nullptr);
                for (int i = 0; i < 5; i++) send_notice(NOTICE_NONE, about);
                return;
            }
            send_notice(NOTICE_LEFT, about);
        } else if (choice == LeftChoice::Computer) {
            uint8_t gone = gone_mask;
            gone_mask = gone_bye = 0;
            for (int p = 1; p < players; p++) {
                if (gone & (1u << p)) leave_session(p, "left");
            }
            for (int i = 0; i < 5; i++) send_notice(NOTICE_NONE, about);
            if (game.show_notice != nullptr) game.show_notice("");
        } else {
            say("[netplay] frame %" PRIu64 ": everyone goes back to the lobby\n", sframe);
            for (int i = 0; i < 10; i++) send_notice(NOTICE_LOBBY, about);
            fflush(stderr);
            close_session_record();
            udp::close();
            game.back_to_menu(0, "", "host");
        }
    }

    // On the host, a player joining again whose copy has finished its hellos: every copy goes back to the host's party
    // save (the last scene's start) after the same frame, the player's own with it, and plays on from there with the
    // player back in place of the computer (a resync, whose save carries the ports given back).
    void check_rejoins() {
        if (resyncing) {
            return; // after the resync under way
        }
        for (int p = 1; p < players; p++) {
            Remote& g = remotes[p];
            if (!g.rejoining || !g.identity_ok || !g.saw_ours) {
                continue;
            }
            if (!pack_save()) {
                say("[netplay] player %d can't join again: the party has no save yet\n", p + 1);
                g.rejoining = false;
                continue;
            }
            uint8_t bit = uint8_t(1u << p);
            if (playing(g)) {
                // Gone, while the game waited for it: its inputs end where they stopped.
                g.left_from = std::max(g.next, uint32_t(first_delay));
                g.left_until = UINT32_MAX;
            }
            if (gone_mask & bit) {
                gone_mask &= uint8_t(~bit);
                gone_bye &= uint8_t(~bit);
                if (gone_mask == 0) {
                    if (game.ask_left != nullptr) game.ask_left(-1, nullptr);
                    for (int i = 0; i < 5; i++) send_notice(NOTICE_NONE, p);
                }
            }
            resync_at = std::max<uint32_t>(frames_sent_to, wholes_ready());
            uint32_t back_at = resync_at + 1;
            // Its inputs until then are empty, and it plays from back_at.
            if (g.inputs.size() < back_at) {
                g.inputs.resize(back_at);
                g.have.resize(back_at, 0);
            }
            for (uint32_t f = 0; f < back_at; f++) {
                if (!g.have[f]) {
                    g.inputs[f] = Inputs{};
                    g.have[f] = 1;
                }
            }
            g.next = std::max(g.next, back_at);
            while (g.next < g.have.size() && g.have[g.next]) {
                g.next++;
            }
            g.ack = back_at;
            g.left_until = back_at;
            g.rejoining = false;
            g.hashes.clear();
            g.compare_from = back_at;
            g.heard = Clock::now();
            resync_back = bit;
            resyncing = true;
            for (int q = 1; q < players; q++) {
                remotes[q].resync_ready = false;
            }
            say("[netplay] frame %" PRIu64 ": player %d joins again: every copy goes back to the save of frame %" PRIu64
                " after frame %u\n", sframe, p + 1, resync_head.frame, resync_at);
            if (game.show_resync != nullptr) {
                game.show_resync(true);
            }
            return; // one at a time
        }
    }

    // On a guest joining a session in progress, at each of its frames until it has the host's save: the game plays
    // for itself meanwhile, with no one's inputs, and once the save is here the copy goes to the frame after the one
    // the copies resync after, its buffers as if it had played to there.
    Frame joining_frame() {
        while (receive_packet(0)) {
        }
        static uint32_t calls = 0;
        if (++calls % 10 == 0) {
            if (resyncing) {
                send_resync_ack();
            } else {
                send_hello(remotes[0]);
            }
        }
        if (Clock::now() - remotes[0].heard > std::chrono::milliseconds(timeout_ms)) {
            say("[netplay] the host stopped answering\n");
            end_session(3);
        }
        Frame fr;
        fr.connected = uint8_t(connected_mask);
        fr.vis = 2;
        return fr;
    }

    // On a guest, the host's KIND_NOTICE.
    uint8_t notice_shown = NOTICE_NONE;
    void take_notice(Reader& r) {
        uint8_t code = r.u8();
        int about = r.u8();
        if (!r.ok || about >= PORTS) {
            return;
        }
        if (code == NOTICE_LOBBY && game.back_to_menu != nullptr) {
            say("[netplay] the host takes everyone back to the lobby\n");
            fflush(stderr);
            close_session_record();
            udp::close();
            game.back_to_menu(0, "", "join");
        }
        if (code == notice_shown || game.show_notice == nullptr) {
            return;
        }
        notice_shown = code;
        if (code == NOTICE_LEFT) {
            std::string name = roster[about].name.empty() ? "Player " + std::to_string(about + 1) : roster[about].name;
            game.show_notice((name + " left the party. Waiting for the host to choose what happens next...").c_str());
        } else {
            game.show_notice("");
        }
    }

    // The copy that has been silent longest, while a frame waits (of the guests still in, on the host).
    const Remote& quietest(int& who) {
        int first = hosting ? 1 : 0, last = hosting ? players : 1;
        who = first;
        for (int p = first; p < last; p++) {
            if (hosting && (!playing(remotes[p]) || (gone_mask & (1u << p)))) {
                continue;
            }
            if ((hosting && !playing(remotes[who])) || remotes[p].heard < remotes[who].heard) {
                who = p;
            }
        }
        return remotes[who];
    }
}

bool netplay::detail::open_session(const SessionSettings& settings) {
    hosting = settings.host;
    if (hosting) {
        // The party's id: the last party's when it resumes, a new one's otherwise.
        resuming = settings.resume && party_resumable(offered);
        if (settings.resume && !resuming) {
            say("[netplay] there's no party to resume: this session starts a new one\n");
        }
        party_id = resuming ? offered.id : (uint64_t(std::random_device{}()) << 32 | std::random_device{}()) | 1;
    }
    if (const char* d = getenv("MP_NETPLAY_DELAY"); d != nullptr && *d != '\0') {
        delay = std::clamp(atoi(d), 1, 15); // the host's, for every player; a guest's is ignored
        delay_forced = true;
    }
    if (const char* f = getenv("MP_NETPLAY_FRAMES")) session_frames = strtoll(f, nullptr, 10);
    if (const char* t = getenv("MP_NETPLAY_TIMEOUT_MS")) timeout_ms = drop_ms = atoi(t);
    if (const char* d = getenv("MP_NETPLAY_DROP")) drop_percent = atoi(d);
    if (const char* j = getenv("MP_NETPLAY_JITTER_MS")) jitter_ms = atoi(j);
    if (const char* l = getenv("MP_NETPLAY_LATENCY_MS")) {
        sscanf(l, "%d:%d:%d:%d:%d", &latency_ms, &latency_jitter_ms, &spike_from_s, &spike_for_s, &spike_ms);
    }
    uint16_t port = settings.port;
    if (settings.via_server && !udp::relay_on()) {
        say("[netplay] this copy isn't on the server %s any more: join it again\n", settings.peer.c_str());
        end_session(5);
    }
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
        if (settings.via_server && udp::relay_on()) {
            remotes[0].endpoint = udp::relay_host(); // the host, through the server the copy joined
        } else if (!udp::resolve(peer, remotes[0].endpoint)) {
            say("[netplay] couldn't find the host at %s (an address, like 203.0.113.5 or 203.0.113.5:7845)\n", peer.c_str());
            end_session(5);
        }
        remotes[0].known = true;
    }
    if (!udp::open(port)) {
        say("[netplay] couldn't open UDP port %u: is another copy of the game using it?\n", unsigned(port));
        end_session(5);
    }
    if (settings.via_server) {
        say(hosting ? "[netplay] hosting on the server %s\n" : "[netplay] joining the party on the server %s\n",
            settings.peer.c_str());
    } else if (hosting && in_lobby) {
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
    if (!receiving) {
        receiving = true;
        std::thread([] {
            for (;;) {
                receive_packet(50);
            }
        }).detach();
    }
    std::unique_lock<std::recursive_mutex> lock(session_mutex);
    if (!record_opened) {
        record_opened = true;
        open_session_record(player, host_save, said);
        said_in_play = said.size();
        for (Remote& r : remotes) {
            r.heard = Clock::now(); // the time from the lobby to the game's first frame doesn't count as silence
        }
    }
    if (joining_now) {
        return joining_frame();
    }
    if (session_frames >= 0 && int64_t(sframe) >= session_frames) {
        // Stay until the others have this copy's part of every frame they play, then end.
        auto until = Clock::now() + std::chrono::seconds(3);
        while (!others_have_all() && Clock::now() < until) {
            send_all();
            receive_packet(16);
        }
        say("[netplay] played %" PRIu64 " frames: %" PRIu64 " hashes compared, every one matched\n",
                sframe, hashes_compared);
        end_session(0);
    }
    local_hashes.push_back(hash);
    desync_frame_step();
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
    adjust_delay();
    push_local(mine, taken.vis);

    while (receive_packet(0)) {
    }
    if (hosting) {
        check_rejoins();
    }
    send_all();
    bool told = false;
    bool held_screen = false;
    auto wait_start = Clock::now();
    auto sent_at = wait_start;
    while (!frame_ready()) {
        session_cv.wait_for(lock, std::chrono::milliseconds(16));
        if (frame_ready() || Clock::now() - sent_at < std::chrono::milliseconds(16)) {
            continue;
        }
        send_all();
        sent_at = Clock::now();
        int who = 0;
        auto silent = Clock::now() - quietest(who).heard;
        if (!told && silent > std::chrono::seconds(1)) {
            say("[netplay] frame %" PRIu64 ": waiting for player %d\n", sframe, who + 1);
            told = true;
        }
        if (hosting) {
            check_left_choice();
            check_rejoins();
        }
        if (game.hold_screen != nullptr && Clock::now() - wait_start > std::chrono::milliseconds(100)) {
            game.hold_screen(true);
            held_screen = true;
        }
        if (hosting && silent > std::chrono::milliseconds(drop_ms)) {
            guest_gone(who, "stopped answering");
        } else if (!hosting && silent > std::chrono::milliseconds(timeout_ms)) {
            say("[netplay] frame %" PRIu64 ": the host stopped answering. The party was saved at the start of the last "
                "scene: the host can pick it up with Resume Party.\n", sframe);
            end_session(3);
        }
    }
    if (held_screen) {
        game.hold_screen(false);
    }
    note_wait(std::chrono::duration<double, std::milli>(Clock::now() - wait_start).count());
    if (hosting) {
        send_frames(); // the frame is whole: the guests can have it now
    }

    Frame fr;
    fr.connected = uint8_t(connected_mask);
    fr.vis = hosting ? local_vis[sframe] : wholes_vis[sframe];
    uint8_t left = hosting ? uint8_t(left_at(uint32_t(sframe)) | (resyncing && sframe == resync_at ? LEFT_RESYNC : 0))
                           : wholes_left[sframe];
    fr.left = left & ~LEFT_RESYNC;
    fr.resync = (left & LEFT_RESYNC) != 0;
    for (int q = 0; q < players; q++) {
        Inputs in = hosting ? input_of(q, uint32_t(sframe)) : wholes[sframe][q];
        for (int i = 0; i < game.samples_per_frame; i++) {
            fr.samples[i][q] = in[i];
        }
    }
    if (fr.resync) {
        finish_resync(); // the copies take up the save at the start of the next frame
    }
    sframe++;
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

bool netplay::detail::join_now(std::vector<uint8_t>& bytes, uint8_t& back) {
    std::lock_guard<std::recursive_mutex> lock(session_mutex);
    if (!joining_now || !resyncing || !resync_ready) {
        return false;
    }
    bytes = resync_bytes;
    back = resync_back;
    uint32_t back_at = resync_at + 1;
    finish_resync(false);
    joining_now = false;
    sframe = back_at;
    local_hashes.assign(back_at, 0);
    local_inputs.assign(size_t(back_at) + size_t(delay), Inputs{});
    local_vis.assign(size_t(back_at) + size_t(delay), 2);
    size_t n = std::max<size_t>(wholes.size(), back_at);
    wholes.resize(n);
    whole_have.resize(n, 0);
    wholes_vis.resize(n, 2);
    wholes_left.resize(n, 0);
    for (uint32_t f = 0; f < back_at; f++) {
        whole_have[f] = 1;
    }
    Remote& host = remotes[0];
    host.next = back_at;
    while (host.next < whole_have.size() && whole_have[host.next]) {
        host.next++;
    }
    host.ack = back_at;
    say("[netplay] joined the party again: playing from frame %u\n", back_at);
    send_all();
    return true;
}

bool netplay::detail::join_wait() {
    std::lock_guard<std::recursive_mutex> lock(session_mutex);
    bool held = false;
    auto asked = Clock::now();
    while (joining_now && !(resyncing && resync_ready)) {
        receive_packet(16);
        if (Clock::now() - asked > std::chrono::milliseconds(150)) {
            asked = Clock::now();
            if (resyncing) {
                send_resync_ack();
            } else {
                send_hello(remotes[0]);
            }
        }
        if (Clock::now() - remotes[0].heard > std::chrono::milliseconds(timeout_ms)) {
            say("[netplay] the host stopped answering\n");
            end_session(3);
        }
        if (game.hold_screen != nullptr) {
            game.hold_screen(true);
            held = true;
        }
    }
    if (held) {
        game.hold_screen(false);
    }
    return joining_now;
}

bool netplay::detail::joining() {
    return joining_now;
}

void netplay::answer_left(LeftChoice choice) {
    left_choice = choice;
    session_cv.notify_all();
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
        if (settings.via_server) {
            status.share.push_back(settings.peer); // the others join the same server
        } else {
            for (const std::string& a : udp::local_addresses()) {
                status.share.push_back(a + ":" + std::to_string(settings.port != 0 ? settings.port : 7845));
            }
        }
    }
    set_status(LobbyStatus::Waiting);
    lobby_thread = std::thread(run_lobby);
    return true;
}

namespace {
    std::mutex server_mutex;
    ServerJoin server_state;           // under server_mutex
    std::atomic<bool> server_cancelled{ false };
    std::thread server_thread;
}

void netplay::server_join(const std::string& address, const std::string& name, const std::string& password) {
    server_cancel();
    {
        std::lock_guard lock{ server_mutex };
        server_state = ServerJoin{};
        server_state.phase = ServerJoin::Joining;
    }
    server_cancelled = false;
    server_thread = std::thread([address, name, password]() {
        bool host = false;
        std::string error;
        bool joined = udp::relay_connect(address, name.substr(0, MAX_NAME), password, host, error, &server_cancelled);
        std::lock_guard lock{ server_mutex };
        server_state.phase = joined ? ServerJoin::Joined : ServerJoin::Failed;
        server_state.host = host;
        server_state.reason = error;
    });
}

bool netplay::via_server() {
    return udp::relay_on();
}

ServerJoin netplay::server_join_status() {
    std::lock_guard lock{ server_mutex };
    return server_state;
}

void netplay::server_cancel() {
    server_cancelled = true;
    if (server_thread.joinable()) {
        server_thread.join();
    }
    std::lock_guard lock{ server_mutex };
    if (server_state.phase == ServerJoin::Joined && !in_lobby) {
        udp::relay_final(); // joined, but its lobby never opened
        udp::close();
    }
    server_state = ServerJoin{};
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

void netplay::leave() {
    goodbye();
    udp::relay_final();
}
