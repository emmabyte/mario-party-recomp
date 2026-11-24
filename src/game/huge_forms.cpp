// Bash 'n' Cash (overlay 0x10) and Coin Block Blitz (overlay 0x14) each put a round shadow (a flat square 120 units
// across) on the ground under what a hit throws into the air, and scale it by that thing's height h as 0.4 - h/2000
// (Bash 'n' Cash's func_800FBCC8, Coin Block Blitz's func_800F83A8). Above a height of 200 the game's branch skips
// that sum and hands h itself on as the scale: 300 to 636 times in Bash 'n' Cash under what a hit on the Bowser Suit
// throws up, 300 to 453 times over 7 frames in Coin Block Blitz under a hit ? block, 36,000 to 76,000 units across,
// where an arena is about 1,200. In Coin Block Blitz its corners reach clip coordinates past 100,000, with some behind
// the eye, which a console's RSP (16.16 fixed point) can't hold; RT64 draws them as they are, in floats. The floor went
// dark with each hit: Bash 'n' Cash's stone and its walls' feet for over half a second, Coin Block Blitz's grass for a
// quarter (there once the decal test took the console's depth tolerance, N64Recomp-browser's
// RT64_RDP_DECAL_TOLERANCE, for the ? blocks' own shadows, which sit up to four depth slopes above the grass and didn't
// draw at all; before that, only a strip at the floor's back passed).
//
// No shadow either minigame draws on purpose is that big (the players' and the blocks' are about 1 times), so a form
// in them scaled past 20 times is drawn as hidden: mp_huge_form_flags in each of the three walks of the engine's forms
// in func_800247FC (the frame's draw), where it has loaded a form's flags (the word at 0x20) and tests their 0x4, a
// hidden form's (us.toml's hooks, after mp_instructions_form_flags). The game's own flags and scale are left as they
// are.

#include "recomp.h"
#include "librecomp/helpers.hpp"

#include <cmath>
#include <cstring>

namespace {
    // omOvlGotoEx's last overlay (src/objmain.c's D_800F09F4), and Bash 'n' Cash's and Coin Block Blitz's.
    constexpr gpr OVERLAY = (int32_t)0x800F09F4;
    constexpr int32_t BASH_N_CASH = 0x10;
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

// mp_huge_form_flags(flags, form): the flags the draw tests the form at this address by.
extern "C" gpr mp_huge_form_flags(uint8_t* rdram, gpr flags, gpr form) {
    int32_t overlay = (int32_t)MEM_W(0, OVERLAY);
    if (overlay != BASH_N_CASH && overlay != COIN_BLOCK_BLITZ) {
        return flags;
    }
    for (int32_t axis = 0; axis < 3; axis++) {
        if (std::fabs(form_float(rdram, form, FORM_SCALE + axis * 4)) > MAX_SCALE) {
            return flags | HIDDEN;
        }
    }
    return flags;
}
