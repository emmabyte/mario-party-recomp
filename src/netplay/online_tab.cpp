// The Online tab in the settings: what the launcher's Online Play screens (online_screens.cpp) keep between runs, the
// player's name, the port a host listens on and the address last joined. Online play starts from those screens, in a
// lobby, before the game starts; the environment starts a session too, for the tests (core/netplay_core.cpp's
// variables).
//
// A guest needs only the host's address: the host gives each guest its player, in the order they join. The host's UDP
// port must be reachable from the guests (forwarded on its router, or over a VPN such as Tailscale, ZeroTier or
// Radmin).

#include <string>

#include "librecomp/config.hpp"
#include "recompui/config.h"
#include "netplay.h"

void mp_netplay::add_settings_tab() {
    recomp::config::Config& online = recompui::config::create_config_tab("Online", "online", false);
    online.add_string_option(
        "name",
        "Your Name",
        "The name the others see in an online party (at most 16 letters). Left empty, you're \"Player\" and your "
        "number.",
        ""
    );
    online.add_string_option(
        "port",
        "Host's Port",
        "Hosting: the UDP port the others reach you on, 7845 unless something else uses it. Over the internet without "
        "a VPN, forward it on your router to this computer.",
        "7845"
    );
    online.add_string_option("address", "Host's Address", "The address last joined.", "", true);
    mp_netplay::set_online_config(&online);
}

bool mp_netplay::available() {
    return true;
}
