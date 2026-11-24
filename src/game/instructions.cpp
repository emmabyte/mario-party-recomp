// A minigame's instructions screen (overlay 0x6F) opens on a cloud with a face (form file 0x90006) that grows out of
// black and turns over until it fills the screen, then fades to the screen's sky. Its setup
// (func_800F6B14_MinigameInstructions) runs that before anything else, but the overlay has already loaded the screen's
// other forms by then: the players and the minigame's props (Peach in the Bowser Suit, the gloves, the chests, the
// shells, the flags), waiting left of the screen to walk on once the sky is up. In 4:3 they are out of the picture
// and the screen is black around the cloud. RT64's Aspect Ratio Expand widens the view to the window's, and in a wider
// window they stood in the black at its left side while the cloud grew.
//
// So while the cloud grows, every other form is drawn as hidden. us.toml's hooks call these natives: mp_instructions_cloud
// where the setup has loaded the cloud (its form's index in v0), mp_instructions_cloud_end where its grow and turn are
// over (L_800F6CFC, which the setup reaches with or without the cloud) and the cloud covers the window, and
// mp_instructions_form_flags in each of the three walks of the engine's forms in func_800247FC (the frame's draw),
// where it has loaded a form's flags (the word at 0x20) and tests their 0x4, a hidden form's: the flags with 0x4 for
// a form other than the cloud. The game's own flags are left as they are, so what its processes show and hide in
// that time is theirs once the cloud is over. A 4:3 picture is the same, as nothing but the cloud is in it then.

#include "recomp.h"
#include "librecomp/helpers.hpp"

namespace {
    // D_800F2B7C, the pointer to the engine's forms, 0xC0 bytes each (the decomp's unk_ovl_2D_struct).
    constexpr gpr FORMS = (int32_t)0x800F2B7C;
    constexpr int32_t FORM_SIZE = 0xC0;
    // omOvlGotoEx's last overlay (src/objmain.c's D_800F09F4), and the instructions screen's.
    constexpr gpr OVERLAY = (int32_t)0x800F09F4;
    constexpr int32_t INSTRUCTIONS = 0x6F;
    // A form's flag that its draw skips it for.
    constexpr gpr HIDDEN = 0x4;

    // The cloud's form (its address in RDRAM) while it grows, 0 otherwise.
    int32_t cloud = 0;
}

// mp_instructions_cloud(form): the setup has loaded the cloud as this form.
extern "C" void mp_instructions_cloud(uint8_t* rdram, gpr form) {
    cloud = (int32_t)MEM_W(0, FORMS) + (int16_t)form * FORM_SIZE;
}

// mp_instructions_cloud_end(): the cloud has grown and turned over, and covers the window.
extern "C" void mp_instructions_cloud_end(uint8_t* rdram) {
    cloud = 0;
}

// mp_instructions_form_flags(flags, form): the flags the draw tests the form at this address by. Off the instructions
// screen (a reset in the cloud's time, say) the cloud is over.
extern "C" gpr mp_instructions_form_flags(uint8_t* rdram, gpr flags, gpr form) {
    if (cloud == 0) {
        return flags;
    }
    if ((int32_t)MEM_W(0, OVERLAY) != INSTRUCTIONS) {
        cloud = 0;
        return flags;
    }
    return (int32_t)form == cloud ? flags : (flags | HIDDEN);
}
