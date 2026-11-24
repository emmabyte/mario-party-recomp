#include "patches.h"

// Cast Aways (overlay 0x2B)'s sky, the picture of blue sky and clouds behind its sea and islands (sprite file
// 0x3F0002, its frame 160x120 drawn at twice its size, 320x240), which its setup (func_800F71AC_CastAways) centres at
// (160, 130): 10 lines below the screen's middle, so it covers lines 10 to 250. Above it the frame shows its clear
// colour, which the overlay's start sets to (0x21, 0x42, 0xD6), a lighter, flat blue than the sky's top: a band across
// the top of the screen, 10 of its 240 lines, that a TV's overscan hid and a window shows, 4:3 or wider.
//
// The setup is the game's but for the sky's height: drawn 13/12 as tall about the same middle, so its top reaches the
// screen's (130 - 60 x 2 x 13/12 = 0). The sky above its clouds is all but one colour, and the clouds come up a few
// lines at the horizon.

#define SKY_SCALE_Y (2.0f * 13.0f / 12.0f)

extern u16 D_800FBB5C_ovl2B;
extern u16 D_800FBB5E_ovl2B;
extern s32 D_800FBA64_ovl2B;
extern s16 D_800FBB60_CastAways;
void func_800F7308_CastAways(void);

RECOMP_PATCH void func_800F71AC_CastAways(omObjData* arg0) {
    s16 sprite;

    sprite = InitSprite(0x3F0002);
    D_800FBB5C_ovl2B = sprite;
    D_800FBB5E_ovl2B = func_80019060(sprite, 0, 1);
    func_80018D44(D_800FBB5E_ovl2B, 0x4008);
    SetBasicSpritePos(D_800FBB5E_ovl2B, 0xA0, 0x82);
    ShowBasicSprite(D_800FBB5E_ovl2B);
    SetBasicSpriteSize(D_800FBB5E_ovl2B, 2.0f, SKY_SCALE_Y);
    func_80017660(0, 0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT);
    func_800176C4(0, 640.0f, 480.0f, 511.0f, 640.0f, 480.0f, 511.0f);
    arg0->model[0] = LoadFormFile(0x3F0000, 0x499);
    arg0->model[3] = func_800174C0(0x3F0001, 0x1499);
    func_80039C48((char*)&D_800FBA64_ovl2B, &D_800FBB60_CastAways);
    func_80026040(arg0->model[3]);
    arg0->func_ptr = &func_800F7308_CastAways;
}
