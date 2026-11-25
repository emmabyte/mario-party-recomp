// Netplay built out (the MP_NETPLAY CMake option off): us.toml's hooks still call these, and each answers as netplay
// does when nothing runs, so the game plays as it does without netplay.

#include "netplay.h"

#include <cstdlib>

#include "recomp.h"

void mp_netplay::init(const ultramodern::input::callbacks_t&, const std::string&) {}
void mp_netplay::start() {}
bool mp_netplay::active() { return false; }
bool mp_netplay::fixed_aspect() { return false; }
void mp_netplay::note_sound_gap() {}
void mp_netplay::leave() {}
void mp_netplay::add_settings_tab() {}
bool mp_netplay::available() { return false; }
void mp_netplay::set_online_config(recomp::config::Config*) {}
void mp_netplay::open_online(std::function<bool()>, std::function<void()>) {}
void mp_netplay::close_online() {}
void mp_netplay::online_returned(const std::string&, const std::string&) {}
bool mp_netplay::returning(std::string&, std::string&) { return false; }
void mp_netplay::restart_to_menu(const std::string&, const char*) { std::_Exit(0); }
bool netplay::connected_override(int, bool*) { return false; }
void netplay::note_connected(int, bool) {}

extern "C" int mp_netplay_si_read(uint8_t*) { return 0; }
extern "C" void mp_netplay_tune_bank(gpr) {}
extern "C" void mp_netplay_audio_got(uint8_t*, gpr) {}
extern "C" void mp_netplay_audio_wait(uint8_t*) {}
extern "C" gpr mp_netplay_ai_length(uint8_t*, gpr length) { return length; }
extern "C" void mp_netplay_loader_send(gpr) {}
extern "C" void mp_netplay_loader_sent(uint8_t*, recomp_context*) {}
extern "C" int mp_netplay_vi_client(uint8_t*, gpr, gpr) { return 0; }
extern "C" void mp_netplay_pad_frame(uint8_t*, recomp_context*) {}
extern "C" gpr mp_netplay_vi_tick(uint8_t*, gpr loaded) { return loaded; }
extern "C" gpr mp_netplay_real_vi(uint8_t*, gpr loaded) { return loaded; }
extern "C" int mp_netplay_si_wait(void) { return 0; }
extern "C" gpr mp_netplay_count(uint8_t*, gpr loaded) { return loaded; }
