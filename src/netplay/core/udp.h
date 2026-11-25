// A UDP socket for netplay's sessions (session.cpp): one bound to a local port, sending to and receiving from the
// other copies by their addresses, on Windows (Winsock) and on POSIX systems alike. The web build has none: open()
// fails there.
//
// Or through a netplay server (relay_connect, src/netplay/server/relay_protocol.h): the socket then talks only to the
// server, which passes each packet on, and the other copies are addresses of the server's own (relay endpoints: the
// host, and each guest by the id the server gave it), which send and receive wrap and unwrap. The session sends and
// receives as it always does. The server's own messages are taken in by receive: a player who left the server
// (relay_take_gone), this copy removed from it (relay_ended).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace netplay::udp {
    // An IPv4 address and port, as the network orders them.
    struct Endpoint {
        uint32_t addr = 0;
        uint16_t port = 0;
        bool operator==(const Endpoint& o) const { return addr == o.addr && port == o.port; }
        bool operator!=(const Endpoint& o) const { return !(*this == o); }
        std::string text() const;
    };

    // Binds a socket to the local port (0 for any). Prints why and returns false if it fails.
    bool open(uint16_t local_port);

    // Resolves "host:port". Prints why and returns false if it fails.
    bool resolve(const std::string& host_port, Endpoint& out);

    // Sends one datagram. A lost or refused send is dropped silently, as UDP's are.
    void send(const Endpoint& to, const void* data, size_t size);

    // Waits up to timeout_ms for a datagram and returns its size, 0 if none came, with where it came from.
    size_t receive(void* data, size_t size, int timeout_ms, Endpoint& from);

    void close();

    // Joins the netplay server at "host" or "host:port" (7845 if none) as name, with its password (empty: none), on a
    // socket of its own (open does nothing more while it is joined): true once the server has taken this copy, with
    // whether it is the party's host (the first to join is). False, with why, if it refused, never answered, or cancel
    // became true. The copy's token, which keeps its place across a restart into the lobby (a held host's place), is
    // MP_NETPLAY_RELAY_TOKEN in the environment (the launcher's, from its settings), made the first time if not.
    bool relay_connect(const std::string& server, const std::string& name, const std::string& password, bool& host,
                       std::string& error, const std::atomic<bool>* cancel);
    bool relay_on();
    // The party's host, through the server, for a guest to send to.
    Endpoint relay_host();
    // Takes the next player the server said has left (as its goodbye): false when there is none.
    bool relay_take_gone(Endpoint& who);
    // Why the server no longer passes this copy's packets (removed by its admin, shut down, the server restarted), or
    // empty.
    std::string relay_ended();
    // The player chooses to leave (Leave, closing the game): tells the server at once, and a host's place isn't held.
    // A close without it says the copy dropped out (a restart back into the lobby), and a host's place is held.
    void relay_final();
    // The round trip to the server in ms, -1 before it is known.
    int relay_rtt_ms();

    // This computer's IPv4 addresses, but loopback and link-local ones: where others may reach it (a VPN's among them).
    std::vector<std::string> local_addresses();
}
