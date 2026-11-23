// The Online tab in the settings: how the next game starts, alone (Off, the default) or online, hosting a session or
// joining one, and the launcher's Online Play, which opens it. A game started with Online set to Host or Join starts
// that session at its entry (mp_netplay::start, src/main/main.cpp's game_entrypoint), unless the environment set one
// (core/netplay_core.cpp's variables, for the tests).
//
// A guest needs only the host's address: the host gives each guest its player, in the order they join, and says how
// many play. The host's UDP port must be reachable from the guests (forwarded on its router, or over a VPN such as
// Tailscale, ZeroTier or Radmin).

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "librecomp/config.hpp"
#include "recompui/config.h"
#include "netplay.h"
#include "core/netplay_core.h"

namespace {
    enum class Online {
        Off,
        Host,
        Join,
        OptionCount
    };
    const std::vector<recomp::config::ConfigOptionEnumOption> online_options = {
        {Online::Off, "Off", "Off"},
        {Online::Host, "Host", "Host"},
        {Online::Join, "Join", "Join"},
    };
    const std::string tab_id = "online";
    recomp::config::Config* online = nullptr;

    uint32_t enum_value(const std::string& id) {
        return std::get<uint32_t>(online->get_option_value(id));
    }

    double number_value(const std::string& id) {
        return std::get<double>(online->get_option_value(id));
    }
}

void mp_netplay::add_settings_tab() {
    online = &recompui::config::create_config_tab("Online", tab_id, false);
    online->add_enum_option(
        "mode",
        "Online Play",
        "How the next game starts."
        "<br/>"
        "<b>Off:</b> on this computer alone. "
        "<b>Host:</b> a session the others join, with you as player 1. "
        "<b>Join:</b> the session a friend hosts, at their address below."
        "<br/>"
        "Everyone needs the same build of the game, the same mods and the same Board Movement and Map Scrolling, and "
        "plays from the host's save (a guest's own save isn't changed).",
        online_options,
        Online::Off
    );
    online->add_number_option(
        "players",
        "Players",
        "Hosting: how many people play, you included. Each plays their own character with their own controller; "
        "the game's own player setup chooses who the computer plays for the rest.",
        2, 4, 1, 0, false, 2
    );
    online->add_string_option(
        "address",
        "Host's Address",
        "Joining: the host's address, as their VPN or their router shows it (like 26.12.34.56, or 26.12.34.56:7845 "
        "for another port). The host gives you your player.",
        ""
    );
    online->add_string_option(
        "port",
        "Host's Port",
        "Hosting: the UDP port the others reach you on, 7845 unless something else uses it. Over the internet without "
        "a VPN, forward it on your router to this computer.",
        "7845"
    );
}

bool mp_netplay::available() {
    return true;
}

void mp_netplay::open_settings_tab() {
    recompui::config::set_tab(tab_id);
    recompui::config::open();
}

bool mp_netplay::start_from_settings() {
    if (online == nullptr) {
        return false;
    }
    netplay::SessionSettings s;
    switch (static_cast<Online>(enum_value("mode"))) {
    case Online::Host:
        s.host = true;
        s.players = int(number_value("players"));
        s.port = uint16_t(std::clamp(atoi(std::get<std::string>(online->get_option_value("port")).c_str()), 1, 65535));
        if (s.port == 1) {
            s.port = 7845; // not a number
        }
        break;
    case Online::Join:
        s.peer = std::get<std::string>(online->get_option_value("address"));
        if (s.peer.empty()) {
            return false;
        }
        break;
    default:
        return false;
    }
    return netplay::set_session(s);
}
