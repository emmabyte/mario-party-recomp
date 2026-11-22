// netplay::udp (udp.h): a socket on Winsock or POSIX's, and none on the web.

#include "udp.h"

#include <cstdio>
#include <cstring>

#if defined(__EMSCRIPTEN__)

bool netplay::udp::open(uint16_t, const std::string&) {
    fprintf(stderr, "[netplay] the web build has no UDP\n");
    return false;
}
void netplay::udp::send(const void*, size_t) {}
size_t netplay::udp::receive(void*, size_t, int) { return 0; }
void netplay::udp::close() {}

#else

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static constexpr socket_t NO_SOCKET = INVALID_SOCKET;
static void close_socket(socket_t s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static constexpr socket_t NO_SOCKET = -1;
static void close_socket(socket_t s) { ::close(s); }
#endif

namespace {
    socket_t sock = NO_SOCKET;
    sockaddr_in peer_addr{};
}

bool netplay::udp::open(uint16_t local_port, const std::string& peer) {
#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "[netplay] Winsock didn't start\n");
        return false;
    }
#endif
    size_t colon = peer.rfind(':');
    if (colon == std::string::npos) {
        fprintf(stderr, "[netplay] the peer %s has no :port\n", peer.c_str());
        return false;
    }
    std::string host = peer.substr(0, colon), port = peer.substr(colon + 1);
    addrinfo hints{}, *found = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &found) != 0 || found == nullptr) {
        fprintf(stderr, "[netplay] can't resolve the peer %s\n", peer.c_str());
        return false;
    }
    memcpy(&peer_addr, found->ai_addr, sizeof(peer_addr));
    freeaddrinfo(found);

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == NO_SOCKET) {
        fprintf(stderr, "[netplay] can't make a UDP socket\n");
        return false;
    }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(local_port);
    if (bind(sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        fprintf(stderr, "[netplay] can't bind UDP port %u\n", unsigned(local_port));
        close_socket(sock);
        sock = NO_SOCKET;
        return false;
    }
    return true;
}

void netplay::udp::send(const void* data, size_t size) {
    if (sock != NO_SOCKET) {
        sendto(sock, static_cast<const char*>(data), int(size), 0, reinterpret_cast<const sockaddr*>(&peer_addr),
               sizeof(peer_addr));
    }
}

size_t netplay::udp::receive(void* data, size_t size, int timeout_ms) {
    if (sock == NO_SOCKET) {
        return 0;
    }
    fd_set set;
    FD_ZERO(&set);
    FD_SET(sock, &set);
    timeval tv{ timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    if (select(int(sock + 1), &set, nullptr, nullptr, &tv) <= 0) {
        return 0;
    }
    sockaddr_in from{};
    socklen_t from_size = sizeof(from);
    int got = recvfrom(sock, static_cast<char*>(data), int(size), 0, reinterpret_cast<sockaddr*>(&from), &from_size);
    if (got <= 0 || from.sin_addr.s_addr != peer_addr.sin_addr.s_addr || from.sin_port != peer_addr.sin_port) {
        return 0;
    }
    return size_t(got);
}

void netplay::udp::close() {
    if (sock != NO_SOCKET) {
        close_socket(sock);
        sock = NO_SOCKET;
    }
}

#endif
