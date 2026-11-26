// The natives the patches call (patches/syms.ld gives each a dummy address that N64Recomp turns into a call by name).
// The process scheduler's are lib/mario-party-engine's src/process_threads.cpp.

#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <vector>

#include "recomp.h"
#include "librecomp/overlays.hpp"
#include "librecomp/helpers.hpp"
#include "../netplay/netplay.h"

namespace mp_display_list {
    float picture_aspect();
}

namespace {
    std::mutex loaded_mutex;
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> loaded; // by RAM: ROM and size
}

// recomp_load_overlays(rom, ram, size): the code sections in that stretch of ROM are now at ram (patches/overlays.c).
extern "C" void recomp_load_overlays(uint8_t* rdram, recomp_context* ctx) {
    u32 rom = _arg<0, u32>(rdram, ctx);
    PTR(void) ram = _arg<1, PTR(void)>(rdram, ctx);
    u32 size = _arg<2, u32>(rdram, ctx);

#ifdef MP_PROCESS_STATS
    if (getenv("MP_PROCESS_LOG") != nullptr) {
        fprintf(stderr, "[processes] overlay ROM %08X at %08X\n", rom, ram);
    }
#endif
    load_overlays(rom, ram, size);
    std::lock_guard lock{ loaded_mutex };
    loaded[uint32_t(ram)] = { rom, size };
}

// The overlays loaded, as (ram, rom, size) triples, for a snapshot of the game (src/netplay/mario_party.cpp), and
// loading them again where a snapshot is taken up.
std::vector<uint32_t> mp_overlays_loaded() {
    std::lock_guard lock{ loaded_mutex };
    std::vector<uint32_t> out;
    for (const auto& [ram, at] : loaded) {
        out.insert(out.end(), { ram, at.first, at.second });
    }
    return out;
}

void mp_overlays_reload(const std::vector<uint32_t>& triples) {
    for (size_t i = 0; i + 2 < triples.size(); i += 3) {
        load_overlays(triples[i + 1], int32_t(triples[i]), triples[i + 2]);
        std::lock_guard lock{ loaded_mutex };
        loaded[triples[i]] = { triples[i + 1], triples[i + 2] };
    }
}

// mp_play_aspect_ratio(): the picture's width over its height for what the patches work out from it that the game plays
// by (patches/background.c's board camera, patches/widescreen.c's widened stages and how far out it shows or hides things,
// patches/cast_aways.c's items): the window's
// (lib/mario-party-engine's mp_aspect_ratio), or 16:9 whatever the window while netplay runs, so that copies with
// windows of different shapes keep the same game. Drawing still follows the window: a wider one shows black past 16:9's
// edges where the game has nothing more, a narrower one less of the board.
extern "C" void mp_play_aspect_ratio(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, mp_netplay::fixed_aspect() ? 16.0f / 9.0f : mp_display_list::picture_aspect());
}
