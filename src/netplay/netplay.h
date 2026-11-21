// Netplay's game side: where the four controllers' inputs come from when they don't come from this computer's pads
// alone, and the per-frame hash of RDRAM that tells whether two copies of the game still agree (netplay.cpp says how).
//
// With no session (the default) nothing here changes how the game plays: each hook returns at once and the game's
// own code runs as it does without netplay.

#pragma once

#include <cstdint>

#include "ultramodern/input.hpp"

namespace netplay {
    // Reads the session from the environment (netplay.cpp's header lists the variables) and keeps the input callbacks
    // the runtime is given, which netplay samples the local pads with.
    void init(const ultramodern::input::callbacks_t& callbacks);

    // Whether a session (a recording, a replay) drives the controllers.
    bool active();

    // get_connected_device_info's answer for a port during a replay, which plays the ports the recording had.
    // Returns false when the game's own answer stands; the recorder notes that answer with note_connected.
    bool connected_override(int port, bool* connected);
    void note_connected(int port, bool connected);
}
