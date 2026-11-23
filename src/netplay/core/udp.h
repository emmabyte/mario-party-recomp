// A UDP socket for netplay's sessions (session.cpp): one bound to a local port, sending to and receiving from the
// other copies by their addresses, on Windows (Winsock) and on POSIX systems alike. The web build has none: open()
// fails there.
#pragma once

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

    // This computer's IPv4 addresses, but loopback and link-local ones: where others may reach it (a VPN's among them).
    std::vector<std::string> local_addresses();
}
