// The netplay server: one party's relay on a computer everyone can reach (a VPS, a home server with its port
// forwarded), so that the players need no port, VPN or forwarding of their own. relay_protocol.h says what it speaks.
// It never runs the game and needs no ROM: it passes each session packet from a guest to the host and from the host to
// a guest, and every copy still plays the game and its lockstep (src/netplay/core/session.cpp), through it.
//
//     netplay-server [--port 7845] [--password <text>] [--max-players 4] [--control <socket>]
//     netplay-server ctl <command...>      # an admin command to the server running on this computer
//
// The password may come from MP_NETPLAY_SERVER_PASSWORD instead, kept out of the process list. The admin's commands
// (help lists them: the players, kick, ban, password, lock, the host's place) come from the server's terminal when it
// has one, and from `ctl`, which hands its words to the server through a Unix socket only its user can open
// (--control, by default netplay-server-<port>.sock in the service's $RUNTIME_DIRECTORY, $XDG_RUNTIME_DIR or /tmp;
// `ctl` looks for the installed service's, /run/netplay-server, too). What the admin changes lasts
// until the server stops; what it starts with is its arguments'. It logs to stdout, which a service's journal keeps.
//
// POSIX only: the server ships for Linux (tools/release_server.sh), and builds on macOS too.

#include "relay_protocol.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace netplay::relay;
using Clock = std::chrono::steady_clock;

#ifndef NETPLAY_SERVER_VERSION
#define NETPLAY_SERVER_VERSION "dev"
#endif

namespace {
    struct Member {
        uint8_t id = 0;          // HOST_ID for the host
        bool host = false;
        uint64_t token = 0;
        sockaddr_in addr{};
        std::string name;
        Clock::time_point joined, heard;
        int rtt_ms = -1;         // the copy's own measure, from its pings
        uint64_t packets_in = 0, packets_out = 0, bytes_in = 0, bytes_out = 0;
    };

    struct Ban {
        uint32_t ip;             // network order
        uint64_t token;
        std::string name;
    };

    int udp_fd = -1;
    int control_fd = -1;
    std::string control_path;
    uint16_t port = DEFAULT_PORT;
    std::string password;
    int max_players = MAX_PLAYERS;
    bool locked = false;
    std::vector<Member> members;
    std::vector<Ban> bans;
    uint64_t held_token = 0;     // the host's place, held for this token until held_until
    std::string held_name;
    Clock::time_point held_until;
    Clock::time_point started = Clock::now();
    volatile sig_atomic_t stopping = 0;
    bool interactive = false;

    void log(const char* fmt, ...) {
        char when[32];
        time_t now = time(nullptr);
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", localtime(&now));
        printf("[%s] ", when);
        va_list args;
        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);
        printf("\n");
        fflush(stdout);
    }

    std::string addr_text(const sockaddr_in& a) {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
        return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
    }

    std::string ip_text(uint32_t ip) {
        in_addr a{};
        a.s_addr = ip;
        char buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &a, buf, sizeof(buf));
        return buf;
    }

    bool same_addr(const sockaddr_in& a, const sockaddr_in& b) {
        return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
    }

    std::string who(const Member& m) {
        std::string n = m.name.empty() ? "(no name)" : m.name;
        return "#" + std::to_string(m.id) + " " + n + (m.host ? " (host)" : "");
    }

    void send_to(const sockaddr_in& to, const Writer& w) {
        sendto(udp_fd, w.buf, w.n, 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    }

    void send_reason(const sockaddr_in& to, uint8_t type, const std::string& reason) {
        Writer w;
        w.header(type);
        w.text(reason, MAX_REASON);
        send_to(to, w);
    }

    Member* by_addr(const sockaddr_in& a) {
        for (Member& m : members) {
            if (same_addr(m.addr, a)) return &m;
        }
        return nullptr;
    }

    Member* by_id(uint8_t id) {
        for (Member& m : members) {
            if (m.id == id) return &m;
        }
        return nullptr;
    }

    Member* host() {
        for (Member& m : members) {
            if (m.host) return &m;
        }
        return nullptr;
    }

    bool holding() {
        return held_token != 0 && Clock::now() < held_until;
    }

    // Takes a member off the server. why: for the log. hold: a host's place is held for it (it dropped out rather than
    // leaving). The others hear it's gone: the host of a guest, the guests of the host.
    void remove(size_t index, const char* why, bool hold) {
        Member m = members[index];
        members.erase(members.begin() + long(index));
        log("%s %s", who(m).c_str(), why);
        Writer w;
        w.header(GONE);
        w.u8(m.id);
        if (m.host) {
            for (Member& g : members) send_to(g.addr, w);
            if (hold) {
                held_token = m.token;
                held_name = m.name;
                held_until = Clock::now() + std::chrono::seconds(HOLD_HOST_S);
                log("the host's place is held for %s for %d seconds", m.name.empty() ? "them" : m.name.c_str(), HOLD_HOST_S);
            }
        } else if (Member* h = host()) {
            send_to(h->addr, w);
        }
    }

    void welcome(const Member& m) {
        Writer w;
        w.header(WELCOME);
        w.u8(m.id);
        w.u8(m.host ? 1 : 0);
        send_to(m.addr, w);
    }

    void on_join(Reader& r, const sockaddr_in& from) {
        uint64_t token = r.u64();
        std::string name = r.text(MAX_NAME);
        std::string pw = r.text(MAX_PASSWORD);
        if (!r.ok || token == 0) {
            return;
        }
        for (const Ban& b : bans) {
            if (b.ip == from.sin_addr.s_addr || b.token == token) {
                send_reason(from, REFUSED, "You're banned from this server.");
                return;
            }
        }
        if (!password.empty() && pw != password) {
            send_reason(from, REFUSED, pw.empty() ? "This server needs a password." : "Wrong password.");
            return;
        }
        // The same copy again: its JOIN repeated, or from a new address (its network changed, it restarted into the
        // lobby): it keeps its place.
        for (Member& m : members) {
            if (m.token == token) {
                if (!same_addr(m.addr, from)) {
                    log("%s is now at %s", who(m).c_str(), addr_text(from).c_str());
                    m.addr = from;
                }
                m.name = name;
                m.heard = Clock::now();
                welcome(m);
                return;
            }
        }
        // Another copy at the same address (a new run of the game on the same port): the old one is gone.
        for (size_t i = 0; i < members.size(); i++) {
            if (same_addr(members[i].addr, from)) {
                remove(i, "was replaced by a new copy at the same address", true);
                break;
            }
        }
        if (locked) {
            send_reason(from, REFUSED, "The server is locked: no one new can join right now.");
            return;
        }
        if (int(members.size()) >= max_players) {
            send_reason(from, REFUSED, "The party on this server is full.");
            return;
        }
        Member m;
        m.token = token;
        m.addr = from;
        m.name = name;
        m.joined = m.heard = Clock::now();
        m.host = host() == nullptr && (!holding() || held_token == token);
        if (m.host) {
            m.id = HOST_ID;
            held_token = 0;
        } else {
            uint8_t id = 1;
            while (by_id(id) != nullptr) id++;
            m.id = id;
        }
        members.push_back(m);
        log("%s joined from %s", who(m).c_str(), addr_text(from).c_str());
        welcome(m);
    }

    void on_datagram(const uint8_t* buf, size_t n, const sockaddr_in& from) {
        Reader r{ buf, n };
        uint8_t version = 0, type = 0;
        if (!read_header(r, version, type)) {
            return;
        }
        if (version != VERSION) {
            if (type == JOIN) {
                send_reason(from, REFUSED, "This server runs another version of online play: the game and the server "
                                           "need to be from the same release.");
            }
            return;
        }
        if (type == JOIN) {
            on_join(r, from);
            return;
        }
        Member* m = by_addr(from);
        if (m == nullptr) {
            if (type != LEAVE) {
                Writer w;
                w.header(UNKNOWN);
                send_to(from, w);
            }
            return;
        }
        m->heard = Clock::now();
        m->packets_in++;
        m->bytes_in += n;
        if (type == DATA) {
            uint8_t to = r.u8();
            if (!r.ok) return;
            // A guest's packets go to the host only, the host's to one guest.
            Member* dest = m->host ? (to == HOST_ID ? nullptr : by_id(to)) : (to == HOST_ID ? host() : nullptr);
            if (dest == nullptr) return;
            Writer w;
            w.header(DATA);
            w.u8(m->id);
            w.bytes(r.rest(), r.left());
            send_to(dest->addr, w);
            dest->packets_out++;
            dest->bytes_out += w.n;
        } else if (type == PING) {
            uint64_t t = r.u64();
            uint16_t rtt = r.u16();
            if (!r.ok) return;
            m->rtt_ms = rtt == 0xFFFF ? -1 : rtt;
            Writer w;
            w.header(PONG);
            w.u64(t);
            send_to(from, w);
        } else if (type == LEAVE) {
            bool final = r.u8() != 0;
            for (size_t i = 0; i < members.size(); i++) {
                if (&members[i] == m) {
                    remove(i, final ? "left" : "dropped out", !final);
                    break;
                }
            }
        }
    }

    void expire() {
        auto now = Clock::now();
        for (size_t i = 0; i < members.size();) {
            if (now - members[i].heard > std::chrono::seconds(TIMEOUT_S)) {
                remove(i, "stopped answering", true);
            } else {
                i++;
            }
        }
        if (held_token != 0 && now >= held_until) {
            held_token = 0;
            log("the host's place is no longer held: the next to join hosts");
        }
    }

    // --- The admin's commands.

    std::string duration(Clock::duration d) {
        long s = long(std::chrono::duration_cast<std::chrono::seconds>(d).count());
        char buf[32];
        if (s >= 3600) snprintf(buf, sizeof(buf), "%ldh%02ldm", s / 3600, (s / 60) % 60);
        else if (s >= 60) snprintf(buf, sizeof(buf), "%ldm%02lds", s / 60, s % 60);
        else snprintf(buf, sizeof(buf), "%lds", s);
        return buf;
    }

    // A member by its id (2 or #2) or its name, ignoring case.
    Member* find(const std::string& what, std::string& error) {
        std::string w = what;
        if (!w.empty() && w[0] == '#') w = w.substr(1);
        if (!w.empty() && std::all_of(w.begin(), w.end(), ::isdigit)) {
            if (Member* m = by_id(uint8_t(atoi(w.c_str())))) return m;
        }
        auto lower = [](std::string s) { for (char& c : s) c = char(tolower(uint8_t(c))); return s; };
        Member* found = nullptr;
        for (Member& m : members) {
            if (lower(m.name) == lower(what)) {
                if (found != nullptr) {
                    error = "more than one player is called " + what + ": use their number from `players`";
                    return nullptr;
                }
                found = &m;
            }
        }
        if (found == nullptr) error = "no player " + what + " (see `players`)";
        return found;
    }

    const char* HELP =
        "players                 who is on the server (also: list, status)\n"
        "kick <player> [why]     removes a player (their number from `players`, or their name); they can join again\n"
        "ban <player> [why]      removes a player and keeps their address out (everyone on their network) until the\n"
        "                        server stops\n"
        "bans                    the bans\n"
        "unban <n|all>           lifts a ban (its number from `bans`), or every one\n"
        "password <text>         joining needs this password from now on (players already in stay)\n"
        "password off            joining needs no password\n"
        "lock / unlock           stops / lets new players join (players already in can still reconnect)\n"
        "max <2-4>               the most players the party takes\n"
        "release                 stops holding the host's place for a host that dropped out: the next to join hosts\n"
        "end                     removes everyone, ending the party\n"
        "stop                    stops the server\n";

    std::string command(const std::string& line) {
        std::istringstream in(line);
        std::string cmd;
        in >> cmd;
        std::string arg;
        in >> arg;
        std::string rest;
        std::getline(in, rest);
        rest.erase(0, rest.find_first_not_of(' '));
        std::ostringstream out;
        if (cmd.empty()) {
            return "";
        } else if (cmd == "help" || cmd == "?") {
            out << HELP;
        } else if (cmd == "players" || cmd == "list" || cmd == "status") {
            out << "netplay server " << NETPLAY_SERVER_VERSION << " on UDP port " << port << ", up "
                << duration(Clock::now() - started) << "\n";
            out << "password " << (password.empty() ? "off" : "on") << ", " << (locked ? "locked" : "open")
                << ", at most " << max_players << " players\n";
            if (members.empty()) {
                out << "no players\n";
            }
            std::vector<const Member*> sorted;
            for (const Member& m : members) sorted.push_back(&m);
            std::sort(sorted.begin(), sorted.end(), [](const Member* a, const Member* b) { return a->id < b->id; });
            for (const Member* m : sorted) {
                char buf[256];
                snprintf(buf, sizeof(buf), "  #%u  %-16s %-5s %-21s ping %s  in %s  idle %s\n", unsigned(m->id),
                         m->name.empty() ? "(no name)" : m->name.c_str(), m->host ? "HOST" : "guest",
                         addr_text(m->addr).c_str(),
                         m->rtt_ms < 0 ? "?" : (std::to_string(m->rtt_ms) + " ms").c_str(),
                         duration(Clock::now() - m->joined).c_str(), duration(Clock::now() - m->heard).c_str());
                out << buf;
            }
            if (host() == nullptr && holding()) {
                out << "the host's place is held for " << (held_name.empty() ? "the last host" : held_name) << " for "
                    << duration(held_until - Clock::now()) << " more\n";
            } else if (host() == nullptr) {
                out << "no host: the next player to join hosts\n";
            }
        } else if (cmd == "kick" || cmd == "ban") {
            if (arg.empty()) return "which player? " + cmd + " <number or name> [why]\n";
            std::string error;
            Member* m = find(arg, error);
            if (m == nullptr) return error + "\n";
            std::string why = rest.empty() ? (cmd == "ban" ? "You were banned from the server."
                                                           : "The server's admin removed you from the party.")
                                           : rest;
            for (int i = 0; i < 3; i++) send_reason(m->addr, KICKED, why);
            if (cmd == "ban") {
                bans.push_back({ m->addr.sin_addr.s_addr, m->token, m->name });
                out << "banned " << who(*m) << " (" << ip_text(m->addr.sin_addr.s_addr) << ")\n";
            } else {
                out << "kicked " << who(*m) << "\n";
            }
            for (size_t i = 0; i < members.size(); i++) {
                if (&members[i] == m) {
                    remove(i, cmd == "ban" ? "was banned" : "was kicked", false);
                    break;
                }
            }
        } else if (cmd == "bans") {
            if (bans.empty()) out << "no bans\n";
            for (size_t i = 0; i < bans.size(); i++) {
                out << "  " << i + 1 << "  " << ip_text(bans[i].ip) << "  "
                    << (bans[i].name.empty() ? "(no name)" : bans[i].name) << "\n";
            }
        } else if (cmd == "unban") {
            if (arg == "all") {
                out << "lifted " << bans.size() << " ban(s)\n";
                bans.clear();
            } else {
                size_t i = size_t(atoi(arg.c_str()));
                if (i < 1 || i > bans.size()) return "no such ban (see `bans`)\n";
                out << "lifted the ban on " << ip_text(bans[i - 1].ip) << "\n";
                bans.erase(bans.begin() + long(i - 1));
            }
        } else if (cmd == "password") {
            std::string pw = arg + (rest.empty() ? "" : " " + rest);
            if (pw.empty()) {
                out << "password " << (password.empty() ? "off" : "on") << " (password <text> sets it, password off clears it)\n";
            } else if (pw == "off") {
                password.clear();
                out << "joining needs no password\n";
            } else {
                password = pw.substr(0, MAX_PASSWORD);
                out << "joining needs the password from now on\n";
            }
            log("the admin turned the password %s", password.empty() ? "off" : "on");
        } else if (cmd == "lock" || cmd == "unlock") {
            locked = cmd == "lock";
            out << (locked ? "locked: no one new can join\n" : "unlocked: players can join\n");
            log("the admin %s the server", locked ? "locked" : "unlocked");
        } else if (cmd == "max") {
            int n = atoi(arg.c_str());
            if (n < 2 || n > MAX_PLAYERS) return "max takes 2 to 4\n";
            max_players = n;
            out << "the party takes at most " << n << " players\n";
        } else if (cmd == "release") {
            held_token = 0;
            out << "the host's place isn't held: the next to join hosts\n";
        } else if (cmd == "end") {
            while (!members.empty()) {
                for (int i = 0; i < 3; i++) send_reason(members[0].addr, KICKED, "The server's admin ended the party.");
                remove(0, "was removed (the party ended)", false);
            }
            held_token = 0;
            out << "the party is over: everyone was removed\n";
        } else if (cmd == "stop" || cmd == "quit") {
            stopping = 1;
            out << "stopping\n";
        } else {
            out << "no command " << cmd << " (help lists them)\n";
        }
        return out.str();
    }

    // --- The control socket, for `netplay-server ctl`.

    // The control socket's folder: the service's (systemd's RuntimeDirectory), the user's, or /tmp.
    std::string default_control_path() {
        std::string name = "/netplay-server-" + std::to_string(port) + ".sock";
        for (const char* var : { "RUNTIME_DIRECTORY", "XDG_RUNTIME_DIR" }) {
            const char* dir = getenv(var);
            if (dir != nullptr && *dir != '\0') return dir + name;
        }
        return "/tmp" + name;
    }

    // For ctl: the server's socket where it is, the installed service's if not this user's (sudo netplay-server ctl).
    std::string find_control_path() {
        struct stat st;
        std::string mine = default_control_path();
        if (stat(mine.c_str(), &st) == 0) return mine;
        std::string service = "/run/netplay-server/netplay-server-" + std::to_string(port) + ".sock";
        if (stat(service.c_str(), &st) == 0) return service;
        return mine;
    }

    bool open_control() {
        if (control_path.size() >= sizeof(sockaddr_un{}.sun_path)) {
            fprintf(stderr, "the control socket's path is too long: %s\n", control_path.c_str());
            return false;
        }
        // A socket left by a server that didn't stop cleanly: one no server answers on can go.
        int probe = socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, control_path.c_str(), sizeof(addr.sun_path) - 1);
        if (connect(probe, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            close(probe);
            fprintf(stderr, "another server already runs with the control socket %s\n", control_path.c_str());
            return false;
        }
        close(probe);
        unlink(control_path.c_str());
        control_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        mode_t old = umask(0177); // only the server's user can open it
        int bound = bind(control_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        umask(old);
        if (bound != 0 || listen(control_fd, 4) != 0) {
            fprintf(stderr, "can't open the control socket %s: %s\n", control_path.c_str(), strerror(errno));
            close(control_fd);
            control_fd = -1;
            return false;
        }
        return true;
    }

    void serve_control_client(int fd) {
        // One command a connection, a line, answered and closed.
        std::string line;
        char c;
        auto until = Clock::now() + std::chrono::seconds(2);
        while (Clock::now() < until && line.size() < 512) {
            pollfd p{ fd, POLLIN, 0 };
            if (poll(&p, 1, 100) <= 0) continue;
            ssize_t got = read(fd, &c, 1);
            if (got <= 0 || c == '\n') break;
            line.push_back(c);
        }
        std::string answer = command(line);
        if (!line.empty()) log("admin: %s", line.rfind("password ", 0) == 0 ? "password ..." : line.c_str());
        (void)!write(fd, answer.data(), answer.size());
        close(fd);
    }

    int run_ctl(int argc, char** argv, int first) {
        std::string line;
        for (int i = first; i < argc; i++) {
            line += (line.empty() ? "" : " ") + std::string(argv[i]);
        }
        if (line.empty()) line = "players";
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, control_path.c_str(), sizeof(addr.sun_path) - 1);
        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            fprintf(stderr, "no server answers on %s (%s). Is it running, as this user, with this --port or "
                            "--control?\n", control_path.c_str(), strerror(errno));
            return 1;
        }
        line += "\n";
        (void)!write(fd, line.data(), line.size());
        char buf[4096];
        ssize_t got;
        while ((got = read(fd, buf, sizeof(buf))) > 0) {
            fwrite(buf, 1, size_t(got), stdout);
        }
        close(fd);
        return 0;
    }

    void usage() {
        printf("netplay server %s: passes online play's packets between the players, so that only this computer needs "
               "a port others can reach\n\n"
               "  netplay-server [--port 7845] [--password <text>] [--max-players 4] [--control <socket>]\n"
               "  netplay-server ctl <command>     an admin command to the running server (help lists them)\n\n"
               "The password can come from MP_NETPLAY_SERVER_PASSWORD instead. Players join with the server's address "
               "(and its port, if not 7845) in Online Play's Play on a Server.\n",
               NETPLAY_SERVER_VERSION);
    }

    void on_signal(int) {
        stopping = 1;
    }
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (const char* pw = getenv("MP_NETPLAY_SERVER_PASSWORD"); pw != nullptr) {
        password = std::string(pw).substr(0, MAX_PASSWORD);
    }
    int ctl_at = -1;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto value = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s needs %s\n", a.c_str(), what);
                exit(2);
            }
            return argv[++i];
        };
        if (a == "ctl") {
            ctl_at = i + 1;
            break;
        } else if (a == "--port" || a == "-p") {
            int p = atoi(value("a port").c_str());
            if (p < 1 || p > 65535) {
                fprintf(stderr, "--port takes 1 to 65535\n");
                return 2;
            }
            port = uint16_t(p);
        } else if (a == "--password") {
            password = value("a password").substr(0, MAX_PASSWORD);
        } else if (a == "--max-players") {
            max_players = std::clamp(atoi(value("2 to 4").c_str()), 2, MAX_PLAYERS);
        } else if (a == "--control") {
            control_path = value("a path");
        } else if (a == "--version") {
            printf("%s\n", NETPLAY_SERVER_VERSION);
            return 0;
        } else if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else {
            fprintf(stderr, "no option %s\n\n", a.c_str());
            usage();
            return 2;
        }
    }
    if (control_path.empty()) {
        control_path = ctl_at >= 0 ? find_control_path() : default_control_path();
    }
    if (ctl_at >= 0) {
        return run_ctl(argc, argv, ctl_at);
    }

    udp_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(port);
    if (udp_fd < 0 || bind(udp_fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        fprintf(stderr, "can't open UDP port %u: %s\n", unsigned(port), strerror(errno));
        return 1;
    }
    if (!open_control()) {
        return 1;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    interactive = isatty(STDIN_FILENO);
    log("netplay server %s on UDP port %u, password %s, at most %d players", NETPLAY_SERVER_VERSION, unsigned(port),
        password.empty() ? "off" : "on", max_players);
    log("admin commands: `%s ctl help`%s", argv[0], interactive ? ", or type them here" : "");

    std::string typed;
    auto next_expire = Clock::now();
    while (!stopping) {
        pollfd fds[3] = { { udp_fd, POLLIN, 0 }, { control_fd, POLLIN, 0 }, { STDIN_FILENO, POLLIN, 0 } };
        int n = poll(fds, interactive ? 3 : 2, 250);
        if (n < 0 && errno != EINTR) {
            break;
        }
        if (n > 0 && (fds[0].revents & POLLIN)) {
            // Everything waiting, before anything else.
            for (int i = 0; i < 256; i++) {
                uint8_t buf[MAX_DATAGRAM];
                sockaddr_in from{};
                socklen_t from_size = sizeof(from);
                ssize_t got = recvfrom(udp_fd, buf, sizeof(buf), MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&from),
                                       &from_size);
                if (got <= 0) break;
                on_datagram(buf, size_t(got), from);
            }
        }
        if (n > 0 && (fds[1].revents & POLLIN)) {
            int client = accept(control_fd, nullptr, nullptr);
            if (client >= 0) serve_control_client(client);
        }
        if (interactive && n > 0 && (fds[2].revents & (POLLIN | POLLHUP))) {
            char buf[256];
            ssize_t got = read(STDIN_FILENO, buf, sizeof(buf));
            if (got <= 0) {
                interactive = false; // the terminal went
            } else {
                typed.append(buf, size_t(got));
                size_t nl;
                while ((nl = typed.find('\n')) != std::string::npos) {
                    std::string line = typed.substr(0, nl);
                    typed.erase(0, nl + 1);
                    fputs(command(line).c_str(), stdout);
                    fflush(stdout);
                }
            }
        }
        if (Clock::now() >= next_expire) {
            expire();
            next_expire = Clock::now() + std::chrono::milliseconds(500);
        }
    }
    // Everyone hears the server is going, rather than waiting for it to go quiet.
    for (const Member& m : members) {
        for (int i = 0; i < 3; i++) send_reason(m.addr, KICKED, "The server shut down.");
    }
    log("stopped");
    close(control_fd);
    unlink(control_path.c_str());
    return 0;
}
