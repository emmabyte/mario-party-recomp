// Netplay for Mario Party (mario_party.cpp), as src/main/main.cpp starts it. The core it runs on, game-agnostic, is
// src/netplay/core; built without netplay (the MP_NETPLAY CMake option off), off.cpp stands in for both.
#pragma once

#include <functional>
#include <string>

#include "ultramodern/input.hpp"

namespace recomp::config {
    class Config;
}

namespace mp_netplay {
    // Reads the session from the environment (core/netplay_core.cpp lists the variables) and keeps the input callbacks
    // the runtime is given. version is the build's, for the handshake.
    void init(const ultramodern::input::callbacks_t& callbacks, const std::string& version);

    // At the game's entry, before it reads its save: a session's handshake, and the host's save for a guest.
    void start();

    // online_tab.cpp: the Online tab in the settings (before recompui::config::finalize), which keeps the name, port
    // and address the Online Play screens use.
    void add_settings_tab();
    // Whether netplay is built in (the MP_NETPLAY CMake option): the launcher shows Online Play only then.
    bool available();
    // online_screens.cpp: the launcher's Online Play screens (hosting or joining a party, and its lobby). can_start
    // says whether the game can start (a ROM is loaded; it tells the player if not), and start_game starts it, as the
    // launcher's Start Game does, once the lobby is ready. close_online goes back to the launcher.
    void set_online_config(recomp::config::Config* config);
    void open_online(std::function<bool()> can_start, std::function<void()> start_game);
    void close_online();

    // Whether an online session is running: the sound keeps a cushion then (src/main/main.cpp).
    bool active();
    // The sound ran dry online (src/main/main.cpp), for session.log's count of gaps.
    void note_sound_gap();
    // As the program closes: tells the other players' copies this one is leaving the session.
    void leave();
}

namespace netplay {
    // Whether a port has a controller during a replay or a session (core/netplay_core.h).
    bool connected_override(int port, bool* connected);
    void note_connected(int port, bool connected);
}
