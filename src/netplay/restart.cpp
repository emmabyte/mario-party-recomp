// Back to the game's menu after a party ends (netplay_core.h's Game back_to_menu): the game can't be unloaded while it
// runs, so the program starts again, in its launcher, which opens Online Play saying why (and the lobby again when the
// host took everyone back to it). What the next start needs goes in the environment, which it inherits:
// MP_NETPLAY_RETURN (why) and MP_NETPLAY_RETURN_LOBBY ("host" or "join"), and returning() takes them out again so that
// they go no further.

#include "netplay.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#include <climits>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace {
    void set_env(const char* name, const std::string& value) {
#ifdef _WIN32
        _putenv_s(name, value.c_str());
#else
        setenv(name, value.c_str(), 1);
#endif
    }

    void unset_env(const char* name) {
#ifdef _WIN32
        _putenv_s(name, "");
#else
        unsetenv(name);
#endif
    }
}

void mp_netplay::restart_to_menu(const std::string& reason, const char* lobby) {
    set_env("MP_NETPLAY_RETURN", reason.empty() ? std::string(" ") : reason);
    set_env("MP_NETPLAY_RETURN_LOBBY", lobby != nullptr ? lobby : "");
    fflush(stdout);
    fflush(stderr);
#ifdef _WIN32
    wchar_t path[4096];
    DWORD n = GetModuleFileNameW(nullptr, path, 4096);
    if (n > 0 && n < 4096) {
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(path, nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
#elif defined(__APPLE__)
    char path[PATH_MAX];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0) {
        execl(path, path, (char*)nullptr);
    }
#else
    execl("/proc/self/exe", "MarioPartyRecompiled", (char*)nullptr);
#endif
    std::_Exit(0);
}

bool mp_netplay::returning(std::string& reason, std::string& lobby) {
    const char* r = getenv("MP_NETPLAY_RETURN");
    if (r == nullptr || *r == '\0') {
        return false;
    }
    reason = r;
    if (reason == " ") {
        reason.clear();
    }
    const char* l = getenv("MP_NETPLAY_RETURN_LOBBY");
    lobby = l != nullptr ? l : "";
    unset_env("MP_NETPLAY_RETURN");
    unset_env("MP_NETPLAY_RETURN_LOBBY");
    return true;
}
