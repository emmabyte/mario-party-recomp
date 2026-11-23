// The Online tab in the settings: how the next game starts, alone (Off, the default) or online, hosting a session or
// joining one, and the launcher's Online Play, which opens it. A game started with Online set to Host or Join starts
// that session at its entry (mp_netplay::start, src/main/main.cpp's game_entrypoint), unless the environment set one
// (core/netplay_core.cpp's variables, for the tests).
//
// The tab is RecompFrontend's options page with a footer of its own: its Start Online Game button saves the options,
// closes the settings and opens the lobby (lobby_view.cpp), where the players gather; the lobby starts the game, as the
// launcher's Start Game does, with the function main.cpp gives open_settings_tab, once the host starts it. Its Config is kept here rather than by create_config_tab, which has no way to add the button,
// so this file loads it (add_settings_tab) and saves it when the tab closes. Once the game has started the button is
// left out: a session starts only with the game.
//
// A guest needs only the host's address: the host gives each guest its player, in the order they join, and says how
// many play. The host's UDP port must be reachable from the guests (forwarded on its router, or over a VPN such as
// Tailscale, ZeroTier or Radmin).

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "librecomp/config.hpp"
#include "recompui/config.h"
#include "recompui/recompui.h"
#include "config/ui_config_page_options_menu.h"
#include "elements/ui_button.h"
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
    std::unique_ptr<recomp::config::Config> online;
    std::function<bool()> can_start;
    std::function<void()> start_game;
    std::atomic<bool> game_started = false;

    uint32_t enum_value(const std::string& id) {
        return std::get<uint32_t>(online->get_option_value(id));
    }

    double number_value(const std::string& id) {
        return std::get<double>(online->get_option_value(id));
    }
}

namespace {
    // The session the options say, if they say one (Host, or Join with an address).
    bool settings_from_options(netplay::SessionSettings& s) {
        if (online == nullptr) {
            return false;
        }
        switch (static_cast<Online>(enum_value("mode"))) {
        case Online::Host:
            s.host = true;
            s.players = int(number_value("players"));
            s.port = uint16_t(std::clamp(atoi(std::get<std::string>(online->get_option_value("port")).c_str()), 1, 65535));
            if (s.port == 1) {
                s.port = 7845; // not a number
            }
            return true;
        case Online::Join:
            s.peer = std::get<std::string>(online->get_option_value("address"));
            return !s.peer.empty();
        default:
            return false;
        }
    }

    void tell(const std::string& text) {
        recompui::open_info_prompt("Online Play", text, "OK", []() {}, recompui::ButtonStyle::Primary);
    }

    // The footer's button: start the game with the options as they are, or say what's missing.
    void press_start() {
        if (game_started) {
            tell("The game has already started. Online play starts with the game: start it again from the launcher.");
            return;
        }
        netplay::SessionSettings s;
        if (!settings_from_options(s)) {
            if (static_cast<Online>(enum_value("mode")) == Online::Join) {
                tell("Type the host's address in Host's Address first.");
            } else {
                tell("Set Online Play to Host or Join first.");
            }
            return;
        }
        online->save_config();
        if (!can_start()) {
            return;
        }
        recompui::config::close();
        mp_netplay::open_lobby(s, start_game, []() {
            recompui::config::set_tab(tab_id);
            recompui::config::open();
        });
    }
}

void mp_netplay::add_settings_tab() {
    online = std::make_unique<recomp::config::Config>("Online", tab_id, false);
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
        2, 4, 1, 0, false, 2, true // hidden: the lobby's Start says how many play; this is for Start Game alone
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
    // Only what the mode needs: the address to join, the port to host on.
    online->add_option_hidden_dependency("address", "mode", Online::Off, Online::Host);
    online->add_option_hidden_dependency("port", "mode", Online::Off, Online::Join);
    online->load_config();

    recompui::config::create_tab(
        "Online",
        tab_id,
        [](recompui::ContextId context, recompui::Element* parent) {
            auto page = context.create_element<recompui::ConfigPageOptionsMenu>(parent, online.get(), true);
            if (game_started || !start_game) {
                return;
            }
            recompui::ConfigHeaderFooter* footer = page->add_footer();
            auto button = context.create_element<recompui::Button>(footer->get_right(), "Start Online Game",
                recompui::ButtonStyle::Primary);
            button->add_pressed_callback(press_start);
        },
        nullptr,
        [](recompui::TabCloseContext) {
            online->save_config();
        }
    );
}

bool mp_netplay::available() {
    return true;
}

void mp_netplay::note_game_started() {
    game_started = true;
}

void mp_netplay::open_settings_tab(std::function<bool()> can, std::function<void()> start) {
    can_start = std::move(can);
    start_game = std::move(start);
    recompui::config::set_tab(tab_id);
    recompui::config::open();
}

bool mp_netplay::start_from_settings() {
    netplay::SessionSettings s;
    return settings_from_options(s) && netplay::set_session(s);
}
