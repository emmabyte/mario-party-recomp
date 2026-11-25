// The relay protocol: how a copy of the game talks to a netplay server (src/netplay/server/server.cpp) that passes a
// session's packets between its players, so that no one has to open a port, forward one on their router or join a
// VPN. Only the server needs a port others can reach. It knows nothing of the game or of the session's own packets
// (src/netplay/core/session.cpp): it passes them on whole, and each copy still runs the game and its lockstep.
//
// A server holds one party. The first player to join it is the party's host, as if they had picked Host a Party, and
// everyone after is a guest of theirs, as if they had joined the host's address. A host that drops out without leaving
// (its program restarting back into the lobby, its network gone a moment) keeps the host's place for HOLD_HOST_S, so
// that it comes back as the host and the others as its guests in whatever order they reconnect.
//
// Every datagram starts with MAGIC and VERSION and a type, then the type's fields, little-endian. A copy's:
// - JOIN: its token (a random number it keeps for the run, and across the program's restart back to the lobby), its
//   player's name and the server's password (empty without one). It sends it until the server answers WELCOME or
//   REFUSED, and again whenever the server says UNKNOWN (its address changed: the token keeps its place).
// - DATA: a session packet for the host (to 0) or, from the host, for a guest (to its id).
// - PING, every second: keeps its place (the server forgets a copy quiet for TIMEOUT_S) and measures the round trip,
//   the last of which it carries for the server's list.
// - LEAVE: it is going. final says the player chose to (Leave, closing the game), so a host's place isn't held.
// The server's:
// - WELCOME: the copy's id (0 the host) and whether it is the host.
// - REFUSED: why it can't join (the password, a full party, a ban, a lock, another version).
// - DATA: a session packet from the host (from 0) or a guest (its id).
// - PONG: the PING's time, echoed.
// - GONE: a player left the server: to the host, a guest; to the guests, the host (0). The session takes it as that
//   player's goodbye.
// - KICKED: the server's admin removed this copy, and why.
// - UNKNOWN: the server doesn't know this address (it restarted, or the copy's address changed): join again.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace netplay::relay {
    constexpr uint32_t MAGIC = 0x4C52504D; // "MPRL"
    constexpr uint8_t VERSION = 1;
    constexpr uint16_t DEFAULT_PORT = 7845;
    constexpr int MAX_PLAYERS = 4;
    constexpr int TIMEOUT_S = 20;
    constexpr int HOLD_HOST_S = 45;
    constexpr size_t MAX_NAME = 16;
    constexpr size_t MAX_PASSWORD = 64;
    constexpr size_t MAX_REASON = 200;
    constexpr size_t HEADER = 6;       // MAGIC, VERSION, type
    constexpr size_t DATA_HEADER = 7;  // and to or from
    constexpr size_t MAX_DATAGRAM = 1600;
    constexpr uint8_t HOST_ID = 0;

    enum Type : uint8_t {
        JOIN = 1,
        DATA = 2,
        PING = 3,
        LEAVE = 4,
        WELCOME = 10,
        REFUSED = 11,
        PONG = 13,
        GONE = 14,
        KICKED = 15,
        UNKNOWN = 16,
    };

    struct Writer {
        uint8_t buf[MAX_DATAGRAM];
        size_t n = 0;
        void u8(uint8_t v) { if (n < sizeof(buf)) buf[n++] = v; }
        void u16(uint16_t v) { u8(uint8_t(v)); u8(uint8_t(v >> 8)); }
        void u32(uint32_t v) { for (int i = 0; i < 4; i++) u8(uint8_t(v >> (8 * i))); }
        void u64(uint64_t v) { u32(uint32_t(v)); u32(uint32_t(v >> 32)); }
        void bytes(const void* p, size_t size) {
            for (size_t i = 0; i < size; i++) u8(static_cast<const uint8_t*>(p)[i]);
        }
        void text(const std::string& s, size_t max) {
            size_t len = s.size() < max ? s.size() : max;
            u8(uint8_t(len));
            bytes(s.data(), len);
        }
        void header(uint8_t type) {
            u32(MAGIC);
            u8(VERSION);
            u8(type);
        }
    };

    struct Reader {
        const uint8_t* p;
        size_t n;
        size_t at = 0;
        bool ok = true;
        uint8_t u8() {
            if (at >= n) { ok = false; return 0; }
            return p[at++];
        }
        uint16_t u16() { uint16_t v = u8(); return uint16_t(v | (u8() << 8)); }
        uint32_t u32() { uint32_t v = 0; for (int i = 0; i < 4; i++) v |= uint32_t(u8()) << (8 * i); return v; }
        uint64_t u64() { uint64_t v = u32(); return v | (uint64_t(u32()) << 32); }
        std::string text(size_t max) {
            size_t len = u8();
            std::string s;
            for (size_t i = 0; i < len && ok; i++) {
                char c = char(u8());
                if (s.size() < max) s.push_back(c >= 32 && c != 127 ? c : '?');
            }
            return s;
        }
        const uint8_t* rest() const { return p + at; }
        size_t left() const { return at <= n ? n - at : 0; }
    };

    // Reads a datagram's header: false if it isn't this protocol's. version is what it says, for a server to tell a
    // copy of another version so.
    inline bool read_header(Reader& r, uint8_t& version, uint8_t& type) {
        if (r.u32() != MAGIC) return false;
        version = r.u8();
        type = r.u8();
        return r.ok;
    }
}
