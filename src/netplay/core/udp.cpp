// netplay::udp (udp.h): a socket on Winsock or POSIX's, and none on the web; and through a netplay server, the relay's
// side of a copy (src/netplay/server/relay_protocol.h).

#include "udp.h"
#include "../server/relay_protocol.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>
#include <random>
#include <thread>

#include <cstdio>
#include <cstdlib>
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
std::vector<std::string> netplay::udp::local_addresses() { return {}; }
bool netplay::udp::relay_connect(const std::string&, const std::string&, const std::string&, bool&, std::string& error,
                                 const std::atomic<bool>*) {
    error = "the web build has no UDP";
    return false;
}
bool netplay::udp::relay_on() { return false; }
netplay::udp::Endpoint netplay::udp::relay_host() { return {}; }
bool netplay::udp::relay_take_gone(Endpoint&) { return false; }
std::string netplay::udp::relay_ended() { return ""; }
void netplay::udp::relay_final() {}
int netplay::udp::relay_rtt_ms() { return -1; }

#else

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static constexpr socket_t NO_SOCKET = INVALID_SOCKET;
static void close_socket(socket_t s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
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

    // Through a netplay server: the socket talks to it alone. A relay endpoint is the server's name for a copy, its id
    // + 1 in port (in host order) with no address, which no real sender has.
    using Clock = std::chrono::steady_clock;
    namespace relay = netplay::relay;
    struct {
        std::atomic<bool> on{ false };
        netplay::udp::Endpoint server;
        uint8_t id = 0;
        bool host = false;
        uint64_t token = 0;
        std::string name, password;
        std::string ended;                    // under mutex
        std::deque<uint8_t> gone;             // under mutex
        std::atomic<bool> final{ false };
        std::atomic<int> rtt_ms{ -1 };
        std::atomic<uint32_t> generation{ 0 }; // the keepalive thread runs while it is the same
        Clock::time_point rejoined{};
        std::mutex mutex;
    } rl;

    netplay::udp::Endpoint relay_endpoint(uint8_t id) {
        netplay::udp::Endpoint e;
        e.addr = 0;
        e.port = uint16_t(id + 1);
        return e;
    }

    bool is_relay_endpoint(const netplay::udp::Endpoint& e) {
        return e.addr == 0 && e.port != 0;
    }

    void send_raw(const netplay::udp::Endpoint& to, const void* data, size_t size);

    void relay_send(const relay::Writer& w) {
        send_raw(rl.server, w.buf, w.n);
    }

    void send_join() {
        relay::Writer w;
        w.header(relay::JOIN);
        w.u64(rl.token);
        w.text(rl.name, relay::MAX_NAME);
        w.text(rl.password, relay::MAX_PASSWORD);
        relay_send(w);
    }

    uint64_t micros() {
        return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
    }

    void end_relay(const std::string& why) {
        std::lock_guard lock{ rl.mutex };
        if (rl.ended.empty()) {
            rl.ended = why;
        }
    }

    // The server's own message, not a session packet: taken in here. Returns the session packet's payload size, and
    // where it starts, for DATA.
    size_t take_relay(const uint8_t* buf, size_t n, size_t& payload_at, netplay::udp::Endpoint& from) {
        relay::Reader r{ buf, n };
        uint8_t version = 0, type = 0;
        if (!relay::read_header(r, version, type) || version != relay::VERSION) {
            return 0;
        }
        switch (type) {
        case relay::DATA: {
            uint8_t id = r.u8();
            if (!r.ok) return 0;
            payload_at = r.at;
            from = relay_endpoint(id);
            return r.left();
        }
        case relay::PONG: {
            uint64_t sent = r.u64();
            if (r.ok) rl.rtt_ms = int(std::min<uint64_t>((micros() - sent) / 1000, 60000));
            return 0;
        }
        case relay::GONE: {
            uint8_t id = r.u8();
            if (r.ok) {
                std::lock_guard lock{ rl.mutex };
                rl.gone.push_back(id);
            }
            return 0;
        }
        case relay::KICKED: {
            std::string why = r.text(relay::MAX_REASON);
            end_relay(why.empty() ? "the server removed you from the party" : why);
            return 0;
        }
        case relay::REFUSED: {
            // A join again (UNKNOWN, below) that the server turned down.
            std::string why = r.text(relay::MAX_REASON);
            end_relay("the server turned you away: " + why);
            return 0;
        }
        case relay::UNKNOWN:
            // The server lost this copy (its address changed, or the server restarted): join again, at most every
            // half second, unless it has left.
            if (!rl.final && Clock::now() - rl.rejoined > std::chrono::milliseconds(500)) {
                rl.rejoined = Clock::now();
                send_join();
            }
            return 0;
        case relay::WELCOME: {
            uint8_t id = r.u8();
            bool host = r.u8() != 0;
            if (r.ok && (id != rl.id || host != rl.host)) {
                end_relay("the server restarted, and the party with it");
            }
            return 0;
        }
        default:
            return 0;
        }
    }

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
    if (is_relay_endpoint(*this)) {
        return port == 1 ? "the host, through the server" : "the server (#" + std::to_string(port - 1) + ")";
    }
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
    if (rl.on && sock != NO_SOCKET) {
        return true; // the server's socket, from relay_connect
    }
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
    if (rl.on) {
        if (!is_relay_endpoint(to) || size > relay::MAX_DATAGRAM - relay::DATA_HEADER) {
            return;
        }
        relay::Writer w;
        w.header(relay::DATA);
        w.u8(uint8_t(to.port - 1));
        w.bytes(data, size);
        relay_send(w);
        return;
    }
    send_raw(to, data, size);
}

namespace {
void send_raw(const netplay::udp::Endpoint& to, const void* data, size_t size) {
    if (sock == NO_SOCKET) {
        return;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = to.addr;
    addr.sin_port = to.port;
    sendto(sock, static_cast<const char*>(data), int(size), 0, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
}
}

namespace {
size_t receive_raw(void* data, size_t size, int timeout_ms, netplay::udp::Endpoint& from);
}

size_t netplay::udp::receive(void* data, size_t size, int timeout_ms, Endpoint& from) {
    if (!rl.on) {
        return receive_raw(data, size, timeout_ms, from);
    }
    // The server's messages go by until a session packet comes, or the time is up.
    auto until = Clock::now() + std::chrono::milliseconds(timeout_ms);
    uint8_t buf[relay::MAX_DATAGRAM];
    for (;;) {
        int left = int(std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count());
        Endpoint sender;
        size_t n = receive_raw(buf, sizeof(buf), std::max(left, 0), sender);
        if (n == 0) {
            return 0;
        }
        if (sender == rl.server) {
            size_t at = 0;
            size_t payload = take_relay(buf, n, at, from);
            if (payload > 0 && payload <= size) {
                memcpy(data, buf + at, payload);
                return payload;
            }
        }
        if (left <= 0) {
            return 0;
        }
    }
}

namespace {
size_t receive_raw(void* data, size_t size, int timeout_ms, netplay::udp::Endpoint& from) {
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
}

void netplay::udp::close() {
    if (rl.on && !rl.final && sock != NO_SOCKET) {
        // The server hears this copy is going: a host that didn't choose to (a restart back into the lobby) keeps its
        // place a while.
        relay::Writer w;
        w.header(relay::LEAVE);
        w.u8(0);
        for (int i = 0; i < 3; i++) {
            relay_send(w);
        }
    }
    rl.on = false;
    rl.final = false;
    rl.generation++;
    {
        std::lock_guard lock{ rl.mutex };
        rl.gone.clear();
        rl.ended.clear();
    }
    if (sock != NO_SOCKET) {
        close_socket(sock);
        sock = NO_SOCKET;
    }
}

bool netplay::udp::relay_connect(const std::string& server, const std::string& name, const std::string& password,
                                 bool& host, std::string& error, const std::atomic<bool>* cancel) {
    close();
    std::string at = server;
    if (at.find(':') == std::string::npos) {
        at += ":" + std::to_string(relay::DEFAULT_PORT);
    }
    if (!resolve(at, rl.server)) {
        error = "couldn't find the server " + server + ": check its address";
        return false;
    }
    if (!open(0)) {
        error = "couldn't open a UDP socket";
        return false;
    }
    // The token: this run's, and the next's when the program restarts into the lobby (restart.cpp keeps the
    // environment).
    const char* t = getenv("MP_NETPLAY_RELAY_TOKEN");
    rl.token = t != nullptr ? strtoull(t, nullptr, 16) : 0;
    if (rl.token == 0) {
        std::random_device rd;
        rl.token = (uint64_t(rd()) << 32 | rd()) | 1;
        char hex[24];
        snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)rl.token);
#if defined(_WIN32)
        _putenv_s("MP_NETPLAY_RELAY_TOKEN", hex);
#else
        setenv("MP_NETPLAY_RELAY_TOKEN", hex, 1);
#endif
    }
    rl.name = name;
    rl.password = password;
    rl.rtt_ms = -1;
    auto until = Clock::now() + std::chrono::seconds(6);
    auto next_join = Clock::now();
    while (Clock::now() < until) {
        if (cancel != nullptr && *cancel) {
            error = "cancelled";
            close();
            return false;
        }
        if (Clock::now() >= next_join) {
            send_join();
            next_join = Clock::now() + std::chrono::milliseconds(250);
        }
        uint8_t buf[relay::MAX_DATAGRAM];
        Endpoint from;
        size_t n = receive_raw(buf, sizeof(buf), 50, from);
        if (n == 0 || from != rl.server) {
            continue;
        }
        relay::Reader r{ buf, n };
        uint8_t version = 0, type = 0;
        if (!relay::read_header(r, version, type)) {
            continue;
        }
        if (type == relay::REFUSED) {
            error = r.text(relay::MAX_REASON);
            close();
            return false;
        }
        if (type == relay::WELCOME && version == relay::VERSION) {
            rl.id = r.u8();
            rl.host = r.u8() != 0;
            if (!r.ok) {
                continue;
            }
            host = rl.host;
            rl.on = true;
            // Pings every second keep this copy's place on the server, whoever is receiving meanwhile.
            uint32_t gen = ++rl.generation;
            std::thread([gen]() {
                while (rl.generation == gen) {
                    relay::Writer w;
                    w.header(relay::PING);
                    w.u64(micros());
                    int rtt = rl.rtt_ms;
                    w.u16(rtt < 0 ? 0xFFFF : uint16_t(rtt));
                    relay_send(w);
                    for (int i = 0; i < 10 && rl.generation == gen; i++) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                }
            }).detach();
            return true;
        }
    }
    error = "the server at " + server + " never answered: check its address, and that it is running";
    close();
    return false;
}

bool netplay::udp::relay_on() {
    return rl.on;
}

netplay::udp::Endpoint netplay::udp::relay_host() {
    return relay_endpoint(relay::HOST_ID);
}

bool netplay::udp::relay_take_gone(Endpoint& who) {
    std::lock_guard lock{ rl.mutex };
    if (rl.gone.empty()) {
        return false;
    }
    who = relay_endpoint(rl.gone.front());
    rl.gone.pop_front();
    return true;
}

std::string netplay::udp::relay_ended() {
    std::lock_guard lock{ rl.mutex };
    return rl.ended;
}

void netplay::udp::relay_final() {
    if (!rl.on || rl.final) {
        return;
    }
    rl.final = true;
    rl.generation++; // no more pings
    relay::Writer w;
    w.header(relay::LEAVE);
    w.u8(1);
    for (int i = 0; i < 3; i++) {
        relay_send(w);
    }
}

int netplay::udp::relay_rtt_ms() {
    return rl.rtt_ms;
}

std::vector<std::string> netplay::udp::local_addresses() {
    std::vector<std::string> out;
    auto add = [&](uint32_t a) { // host order
        if ((a >> 24) == 127 || (a >> 16) == 0xA9FE || a == 0) {
            return; // loopback, link-local
        }
        char buf[24];
        snprintf(buf, sizeof(buf), "%u.%u.%u.%u", a >> 24, (a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF);
        if (std::find(out.begin(), out.end(), buf) == out.end()) {
            out.push_back(buf);
        }
    };
#if defined(_WIN32)
    if (!start()) {
        return out;
    }
    char name[256];
    addrinfo hints{}, *found = nullptr;
    hints.ai_family = AF_INET;
    if (gethostname(name, sizeof(name)) == 0 && getaddrinfo(name, nullptr, &hints, &found) == 0) {
        for (addrinfo* ai = found; ai != nullptr; ai = ai->ai_next) {
            add(ntohl(reinterpret_cast<sockaddr_in*>(ai->ai_addr)->sin_addr.s_addr));
        }
        freeaddrinfo(found);
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) == 0) {
        for (ifaddrs* i = list; i != nullptr; i = i->ifa_next) {
            if (i->ifa_addr != nullptr && i->ifa_addr->sa_family == AF_INET) {
                add(ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr));
            }
        }
        freeifaddrs(list);
    }
#endif
    return out;
}

#endif
