// A UDP socket for netplay's sessions (netplay.cpp): one bound to a local port, sending to and receiving from one
// peer, on Windows (Winsock) and on POSIX systems alike. The web build has none: open() fails there.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace netplay::udp {
    // Binds a socket to the local port (0 for any) and resolves the peer ("host:port"). Prints why and returns false
    // if either fails.
    bool open(uint16_t local_port, const std::string& peer);

    // Sends one datagram to the peer. A lost or refused send is dropped silently, as UDP's are.
    void send(const void* data, size_t size);

    // Waits up to timeout_ms for a datagram from the peer and returns its size, 0 if none came. Datagrams from any
    // other address are dropped.
    size_t receive(void* data, size_t size, int timeout_ms);

    void close();
}
