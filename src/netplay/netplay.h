// Netplay for Mario Party (mario_party.cpp), as src/main/main.cpp starts it. The core it runs on, game-agnostic, is
// src/netplay/core; built without netplay (the MP_NETPLAY CMake option off), off.cpp stands in for both.
#pragma once

#include <functional>
#include <string>

#include "ultramodern/input.hpp"

namespace mp_netplay {
    // Reads the session from the environment (core/netplay_core.cpp lists the variables) and keeps the input callbacks
    // the runtime is given. version is the build's, for the handshake.
    void init(const ultramodern::input::callbacks_t& callbacks, const std::string& version);

    // At the game's entry, before it reads its save: a session's handshake, and the host's save for a guest.
    void start();

    // online_tab.cpp: the Online tab in the settings (before recompui::config::finalize), the launcher's way to it,
    // and the session it says to start, if any (start does this).
    void add_settings_tab();
    // Whether netplay is built in (the MP_NETPLAY CMake option): the launcher shows Online Play only then.
    bool available();
    // Opens the Online tab from the launcher; start starts the game, as the launcher's Start Game does, for the tab's
    // Start Online Game button.
    void open_settings_tab(std::function<void()> start);
    bool start_from_settings();
    // At the game's entry (start): the Online tab leaves its Start Online Game button out from then on.
    void note_game_started();

    // Whether an online session is running: the sound keeps a cushion then (src/main/main.cpp).
    bool active();
}

namespace netplay {
    // Whether a port has a controller during a replay or a session (core/netplay_core.h).
    bool connected_override(int port, bool* connected);
    void note_connected(int port, bool connected);
}
