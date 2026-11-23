// netplay::udp (udp.h): a socket on Winsock or POSIX's, and none on the web.

#include "udp.h"

#include <cstdio>
#include <cstring>

#if defined(__EMSCRIPTEN__)

std::string netplay::udp::Endpoint::text() const { return ""; }
bool netplay::udp::open(uint16_t) {
    fprintf(stderr, "[netplay] the web build has no UDP\n");
    return false;
}
bool netplay::udp::resolve(const std::string&, Endpoint&) { return false; }
void netplay::udp::send(const Endpoint&, const void*, size_t) {}
size_t netplay::udp::receive(void*, size_t, int, Endpoint&) { return 0; }
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
    bool started = false;

    bool start() {
#if defined(_WIN32)
        if (!started) {
            WSADATA wsa;
            if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
                fprintf(stderr, "[netplay] Winsock didn't start\n");
                return false;
            }
        }
#endif
        started = true;
        return true;
    }
}

std::string netplay::udp::Endpoint::text() const {
    uint32_t a = ntohl(addr);
    char buf[32];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u", a >> 24, (a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF, unsigned(ntohs(port)));
    return buf;
}

bool netplay::udp::resolve(const std::string& host_port, Endpoint& out) {
    if (!start()) {
        return false;
    }
    size_t colon = host_port.rfind(':');
    if (colon == std::string::npos) {
        fprintf(stderr, "[netplay] %s has no :port\n", host_port.c_str());
        return false;
    }
    std::string host = host_port.substr(0, colon), port = host_port.substr(colon + 1);
    addrinfo hints{}, *found = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &found) != 0 || found == nullptr) {
        fprintf(stderr, "[netplay] can't resolve %s\n", host_port.c_str());
        return false;
    }
    const sockaddr_in* in = reinterpret_cast<const sockaddr_in*>(found->ai_addr);
    out.addr = in->sin_addr.s_addr;
    out.port = in->sin_port;
    freeaddrinfo(found);
    return true;
}

bool netplay::udp::open(uint16_t local_port) {
    if (!start()) {
        return false;
    }
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

void netplay::udp::send(const Endpoint& to, const void* data, size_t size) {
    if (sock == NO_SOCKET) {
        return;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = to.addr;
    addr.sin_port = to.port;
    sendto(sock, static_cast<const char*>(data), int(size), 0, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
}

size_t netplay::udp::receive(void* data, size_t size, int timeout_ms, Endpoint& from) {
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
    sockaddr_in addr{};
    socklen_t addr_size = sizeof(addr);
    int got = recvfrom(sock, static_cast<char*>(data), int(size), 0, reinterpret_cast<sockaddr*>(&addr), &addr_size);
    if (got <= 0) {
        return 0;
    }
    from.addr = addr.sin_addr.s_addr;
    from.port = addr.sin_port;
    return size_t(got);
}

void netplay::udp::close() {
    if (sock != NO_SOCKET) {
        close_socket(sock);
        sock = NO_SOCKET;
    }
}

#endif
