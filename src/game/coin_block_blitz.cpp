// Coin Block Blitz (overlay 0x14): when a ? block is hit, the minigame puts one of two forms on the ground under it,
// the round shadow (a flat square 120 units across, drawn as a decal on the grass), and scales it from 300 to 453
// times over 7 frames, then hides it again: 36,000 to 54,000 units across, where the arena is about 1,200. Its
// corners reach clip coordinates past 100,000, with some behind the eye, which a console's RSP (16.16 fixed point)
// can't hold; RT64 draws them as they are, in floats, and the whole floor went dark for a quarter of a second with each
// hit. It showed once the decal test took the console's depth tolerance (N64Recomp-browser's RT64_RDP_DECAL_TOLERANCE,
// for the ? blocks' own shadows, which sit up to four depth slopes above the grass and didn't draw at all); before
// that, only a strip at the floor's back passed.
//
// No shadow the minigame draws on purpose is that big (the players' and the blocks' are about 1 times), so a form in
// it scaled past 20 times is drawn as hidden: mp_coin_block_blitz_form_flags in each of the three walks of the
// engine's forms in func_800247FC (the frame's draw), where it has loaded a form's flags (the word at 0x20) and tests
// their 0x4, a hidden form's (us.toml's hooks, after mp_instructions_form_flags). The game's own flags and scale are
// left as they are.

#include "recomp.h"
#include "librecomp/helpers.hpp"

#include <cmath>
#include <cstring>

namespace {
    // omOvlGotoEx's last overlay (src/objmain.c's D_800F09F4), and Coin Block Blitz's.
    constexpr gpr OVERLAY = (int32_t)0x800F09F4;
    constexpr int32_t COIN_BLOCK_BLITZ = 0x14;
    // A form's scale, three floats (the decomp's unk_3C, unk_40 and unk_44).
    constexpr int32_t FORM_SCALE = 0x3C;
    // A form's flag that its draw skips it for.
    constexpr gpr HIDDEN = 0x4;
    // Past this many times its model's size, a form is no shadow the minigame means to draw.
    constexpr float MAX_SCALE = 20.0f;

    float form_float(uint8_t* rdram, gpr form, int32_t offset) {
        int32_t word = MEM_W(offset, form);
        float value;
        std::memcpy(&value, &word, sizeof(value));
        return value;
    }
}

// mp_coin_block_blitz_form_flags(flags, form): the flags the draw tests the form at this address by.
extern "C" gpr mp_coin_block_blitz_form_flags(uint8_t* rdram, gpr flags, gpr form) {
    if ((int32_t)MEM_W(0, OVERLAY) != COIN_BLOCK_BLITZ) {
        return flags;
    }
    for (int32_t axis = 0; axis < 3; axis++) {
        if (std::fabs(form_float(rdram, form, FORM_SCALE + axis * 4)) > MAX_SCALE) {
            return flags | HIDDEN;
        }
    }
    return flags;
}
