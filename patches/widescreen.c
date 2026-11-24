// What a picture wider than 4:3 needs besides RT64's Aspect Ratio Expand, which widens the 3D: a camera's view drawn to
// the window's edges, its 3D moving with the board under it. The engine's patches/camera_view.c says how; here are
// Mario Party's two functions that start a camera's view, src/1DE40.c's func_8001D658 (its viewport and projection)
// and func_8001D7DC (its scissor). A board pans its 3D by moving its camera's viewport (src/48D90.c's func_8004B208).

#include "patches.h"
#include "../lib/mario-party-engine/patches/camera_view.h"
#include "../lib/mario-party-engine/patches/board_back.h"
#include "rt64_extended_gbi.h"

static s32 widened(unk_Struct00* camera) {
    return camera_view_widened(camera->unkD8.x, camera->unkD8.z);
}

RECOMP_PATCH void func_8001D658(s16 index, Gfx** gfx) {
    unk_Struct00* camera = &D_800C3110[index];
    Mtx* projection = &camera->unkF8 + (D_800F3FA8 * 2);
    unk_Struct00* frame = (void*)camera + (D_800F3FA8 * 16);
    Vp* viewport = (Vp*)(&camera->unk58 + (D_800F3FA8 * 8));

    frame->unk58 = camera->unk24.x;
    frame->unk5A = camera->unk24.y;
    frame->unk5C = camera->unk24.z;
    frame->unk60 = camera->unk30.x;
    frame->unk62 = camera->unk30.y;
    frame->unk64 = camera->unk30.z;

    // @recomp A widened camera's viewport in the middle, what it was moved by in its projection.
    camera_view_projection(gfx, index, viewport, camera->unk4C, projection, widened(camera));
    gSPMatrix((*gfx)++, (void*)projection + 0x80000040, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
}

RECOMP_PATCH void func_8001D7DC(s16 index, Gfx** gfx) {
    unk_Struct00* camera = &D_800C3110[index];

    // @recomp The whole screen, or the border, to the window's edges.
    camera_view_scissor(gfx, camera->unkD8.x, camera->unkD8.y, camera->unkD8.z, camera->unkD8.w);
}

// The scenes the game draws at 4:3 whatever the window's width, by their overlay (OvlLoad, overlays.c, calls
// widescreen_overlay as each loads): the engine keeps black at the window's sides while one plays (board_back.h's
// board_back_scene_widest, at 4:3), and the camera's view and the HUD stay inside 4:3.
// - 0x01, Chance Time: its backdrop, the sky and the sand, is a 2D picture of the screen's size, and its camera's
//   scissor (0, 0, 319, 239) would otherwise be widened as the whole screen's, its 3D out over the black beside it.
// - 0x03, Buried Treasure: its dirt is a 2D picture of the screen's size (sprite 0x150001, drawn by
//   func_800F9700_BuriedTreasure's rectangles, below) that the digging takes pixels out of where a player is
//   (func_800F92D4_BuriedTreasure), the players kept inside 4:3 (func_800F8C84_BuriedTreasure). Stretched over a wide
//   window, each tunnel showed further from the middle than the player digging it, by as much more as the window is
//   wider than 4:3.
// - 0x04, Treasure Divers, and 0x30, Deep Sea Divers: the sky, the sea and the reef far off are a 2D picture of the
//   screen's size, and the sea the players swim in, its surface, the sand and the rocks, is 3D that ends at 4:3's
//   edges. Stretched over a wide window, the picture's sea went on past them beside the water's tinted box, its
//   horizon and reef no longer meeting the 3D's.
// - 0x21, Tipsy Tourney: its clouds are a 2D picture of the screen's size behind the four players' views, and they
//   and the paste timer's lines end at 4:3's edges, so past them the window would show the clear's white and the
//   views' 3D.
// - 0x81, the title: its picture, the logo and the characters on white, is a 2D picture of the screen's size.
//   Stretched over a wide window, the characters and the logo were drawn as much wider as the window is.
static const u8 overlays_4x3[] = { 0x01, 0x03, 0x04, 0x21, 0x30, 0x81 };

// The minigames whose 3D ends short of a wide window's edges, and the widest each is drawn, its width over its height:
// where its stage, floor or sea ends at its narrowest across the minigame's play, at a 32:9 window. The engine draws one
// to the window's edges up to that width and black past it (board_back_scene_widest), the camera's view stopping there,
// so a window no wider shows it as before, and a wider one shows black bars rather than the frame's clear around the
// stage's edges. A floor seen in perspective is narrowest at its far edge, and the widest is that edge's. Three seas
// are widened past theirs once each is made (widen_object and widen_model, below).
// - 0x0C, Pedal Power: the back wall, off the middle, ends at 1.8 on its right.
// - 0x11, Bowl Over: the lane in the lower view ends at 3.1.
// - 0x12, Ground Pound: the hill's top edge, 2.05.
// - 0x17, Box Mountain Mayhem: the floor's near corners, 1.85.
// - 0x1A, Mushroom Mixup: the sea's far edge, off the middle as the camera turns, 1.55 on its right, its sea widened.
// - 0x1B, Hammer Drop: the sea's far edge, 1.65, its sea widened.
// - 0x1C, Grab Bag: the floor's near edge, 2.65 (narrower in the opening zoom, under START).
// - 0x1E, Bumper Balls: the sea's far edge, 1.6, its sea widened.
// - 0x1F, Tightrope Treachery: the sea's far edge under the sky, 1.8.
// - 0x23, Crane Game: the cabinet's back wall, 1.6.
// - 0x24, Flower Shower: the field's edge, 2.15.
// - 0x26, Mario Bandstand: the stage's back wall, 1.55.
// - 0x2A, the 2 VS 2 field's near edge, 1.7.
typedef struct {
    u8 overlay;
    f32 widest;
} ShortScene;

static const ShortScene short_scenes[] = {
    { 0x0C, 1.8f },  { 0x11, 3.1f },  { 0x12, 2.05f }, { 0x17, 1.85f }, { 0x1A, 1.55f }, { 0x1B, 1.65f }, { 0x1C, 2.65f },
    { 0x1E, 1.6f },  { 0x1F, 1.8f },  { 0x23, 1.6f },  { 0x24, 2.15f }, { 0x26, 1.55f }, { 0x2A, 1.7f },
};

// The scenes whose 3D in the screen's bottom lines shows what a TV's overscan hid, by their overlay: the engine cuts
// their 3D at the console's overscan border's bottom edge (camera_view.h's camera_view_border_bottom), so the picture
// behind it shows in the 12 lines under it, the rest of the 3D still out to the window's edges.
// - 0x7B, Mini-Game Stadium's results after each minigame, and 0x80, its results at the end: the camera keeps the
//   whole screen every camera starts with (InitCameras), and each player's coins are a column of coins that rises out
//   of the podium as they are counted. The column goes on down through the podium and out under it, and the podiums'
//   feet are at the border's bottom edge, so under them the window showed the columns' lower ends.
static const u8 overlays_border_bottom[] = { 0x7B, 0x80 };

void widescreen_overlay(s32 overlay) {
    f32 widest = 0.0f;
    s32 border_bottom;
    u32 i;

    for (i = 0; i < sizeof(overlays_4x3); i++) {
        if (overlays_4x3[i] == overlay) {
            widest = 4.0f / 3.0f;
        }
    }
    for (i = 0; i < sizeof(short_scenes) / sizeof(short_scenes[0]); i++) {
        if (short_scenes[i].overlay == overlay) {
            widest = short_scenes[i].widest;
        }
    }
    board_back_scene_widest(widest);
    border_bottom = FALSE;
    for (i = 0; i < sizeof(overlays_border_bottom); i++) {
        if (overlays_border_bottom[i] == overlay) {
            border_bottom = TRUE;
        }
    }
    camera_view_border_bottom(border_bottom);
}

// The sprites the sprite manager has loaded, each's first frame's texels first.
typedef struct {
    u8* texels;
} LoadedFrame;
typedef struct {
    LoadedFrame* frame;
} LoadedSprite;
extern LoadedSprite* D_800EC700[];
extern s16 D_800FC198_BuriedTreasure[6];

// Buried Treasure's dirt, a sprite of 320x244 the game draws as 61 rectangles 4 lines high, each the screen's width.
// RT64 stretches a rectangle that covers the screen's whole width over a wide window by itself, and the scene's black
// sides covered only so much of it: over a window as wide as 32:9 the dirt still showed stretched inside 4:3, its
// tunnels away from the players digging them. Its rectangles are kept to 4:3 (G_EX_ASPECT_ADJUST), as the console
// draws them. With their three commands the list is 622 of the 1024 the game allocates for it.
RECOMP_PATCH void func_800F9700_BuriedTreasure(Gfx* arg0) {
    u8* timg = D_800EC700[D_800FC198_BuriedTreasure[1]]->frame->texels;
    s16 i;

    gDPPipeSync(arg0++);
    gDPSetCycleType(arg0++, G_CYC_1CYCLE);
    gSPSetGeometryMode(arg0++, G_TEXTURE_GEN_LINEAR);
    gDPSetCombineMode(arg0++, G_CC_DECALRGBA, G_CC_DECALRGBA);
    gDPSetRenderMode(arg0++, G_RM_ZB_XLU_SURF, G_RM_NOOP2);
    gDPSetTextureFilter(arg0++, G_TF_POINT);
    gDPSetTexturePersp(arg0++, G_TP_NONE);
    gDPSetColorDither(arg0++, G_CD_DISABLE);

    // @recomp The rectangles kept to 4:3.
    gEXEnable(arg0++);
    gEXSetRectAspect(arg0++, G_EX_ASPECT_ADJUST);

    for (i = 0; i < 61; i++) {
        gDPLoadTextureTile(arg0++, timg, G_IM_FMT_RGBA, G_IM_SIZ_16b, 320, 0, 0, i * 4, 319, i * 4 + 3, 0,
                           G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK, 2, G_TX_NOLOD,
                           G_TX_NOLOD);
        gSPTextureRectangle(arg0++, 0, (i * 4) * 4, 320 * 4, (i * 4 + 4) * 4, G_TX_RENDERTILE, 0, 0, 0x0400, 0x0400);
    }

    gEXSetRectAspect(arg0++, G_EX_ASPECT_AUTO);
    gSPEndDisplayList(arg0++);
}

f32 mp_aspect_ratio(void);

// Piranha's Pursuit (overlay 0x31) in a picture wider than 4:3. Its ground, the dirt track and the grass behind it, is
// one object (its models from file 0x450000, set up in func_800FA878_PiranhasPursuit) that the game moves to the
// track's scroll each frame and slides only the texture of (func_800FAEE8_PiranhasPursuit: the scroll over 24 times
// its x scale), and the camera aims up to 300 units behind it, so at 4:3 its left end is just past the picture's edge
// and a wider one showed black beside it, below the jungle. us.toml's hook calls this each frame before the game
// reads the ground's x scale: the ground as many times wider as the picture is than 4:3, which reaches as far past
// each side as the wider view does (its texture stretched as much), and the game's own scale of 1 at 4:3.
void widescreen_piranhas_ground(omObjData* ground) {
    ground->scale.x = mp_aspect_ratio() * (3.0f / 4.0f);
}

// The game slides that texture by writing new texture coordinates into the ground's vertices each frame
// (func_80027C1C), which the renderer's in-between frames cannot carry on, so at a display rate above the game's 30
// the track and the grass behind the ferns stepped at 30 a second while the trees, the ferns and the players on
// them, moved by their matrices, glided. Instead the texture is left where the model has it (us.toml's hook zeroes
// the slide) and the ground is moved: three pieces, the game's own and two copies of it, each its own width apart,
// placed where the game's slid texture stands, which is the scroll times 1 less a texel's 3300 * 32 / 4095 units
// over 24 (the texture moves a little behind the scroll), so the track looks as it did on every frame the game
// draws. The pieces are at that place plus a whole number of widths, the one behind the scroll, the one under it and
// the one ahead; piece n always the same of the three (n modulo 3), so when the scroll passes half a width only the
// one left behind moves, three widths on to the front. Its texture repeats every half width, so in the one frame
// the renderer draws it halfway there, one and a half widths on, it lies on the others with the same texture. The
// game's own piece is placed through the object, whose position the game gives the model after this (and takes
// back from the scroll at the next frame's start); us.toml's hook calls this as the frame function returns. The
// copies are made with the trees' (widescreen_piranhas_trees_copied).
#define PIRANHAS_GROUND_WIDTH 3300.0f
#define PIRANHAS_GROUND_TEXEL (3300.0f * 32.0f / 4095.0f)
#define PIRANHAS_GROUND_SLIDE 24.0f

static s16 piranhas_ground_copies[2];

void widescreen_piranhas_ground_placed(omObjData* ground) {
    unk_ovl_2D_struct* own = &D_800F2B7C[ground->model[0]];
    f32 width = PIRANHAS_GROUND_WIDTH * ground->scale.x;
    f32 scroll = ground->trans.x;
    f32 texture = scroll * (1.0f - PIRANHAS_GROUND_TEXEL / PIRANHAS_GROUND_SLIDE);
    f32 under = (scroll - texture) / width;
    s32 nearest = (s32) (under < 0.0f ? under - 0.5f : under + 0.5f);
    s32 n;

    for (n = nearest - 1; n <= nearest + 1; n++) {
        s32 piece = ((n % 3) + 3) % 3;
        f32 x = texture + width * n;
        unk_ovl_2D_struct* copy;

        if (piece == 0) {
            ground->trans.x = x;
            continue;
        }
        if (piranhas_ground_copies[piece - 1] < 0) {
            continue;
        }
        copy = &D_800F2B7C[piranhas_ground_copies[piece - 1]];
        copy->unk_24 = x;
        copy->unk_28 = own->unk_28;
        copy->unk_2C = own->unk_2C;
        copy->unk_30 = own->unk_30;
        copy->unk_34 = own->unk_34;
        copy->unk_38 = own->unk_38;
        copy->unk_3C = ground->scale.x;
        copy->unk_40 = own->unk_40;
        copy->unk_44 = own->unk_44;
    }
}

// Its 14 trees and bushes along the back of the track, forms 1 to 14 of the same object, which the game keeps within
// the track's scroll plus or minus 1500: each frame (func_800FAEE8_PiranhasPursuit's loop at 0x800FAFA8) it moves one
// that has fallen behind the scroll minus 1500 on by 3000, so they repeat every 3000. With the camera aimed up to 300
// behind the scroll, a picture wider than about 2:1 reached past the ring's left end, the jungle's 2D backdrop alone
// beyond it. A copy of each a ring's width to its left and one to its right, placed each frame where the game's own one
// is, carries them on as they repeat, to the scroll plus or minus 4500, past a 32:9 picture's edges. us.toml's hooks
// call these once the setup (func_800FA878_PiranhasPursuit) has made the 14, and after the frame's loop has moved them.
// At 4:3 the copies are out of the view. They and the ground's two take 30 of the game's 128 forms, of which the
// minigame uses 92.
#define PIRANHAS_TREES 14
#define PIRANHAS_RING 3000.0f

static s16 piranhas_copies[PIRANHAS_TREES][2];

void widescreen_piranhas_trees_copied(omObjData* object) {
    s32 i;
    s32 side;

    for (i = 0; i < PIRANHAS_TREES; i++) {
        for (side = 0; side < 2; side++) {
            piranhas_copies[i][side] = func_80023FC8(object->model[1 + i]);
        }
    }
    for (side = 0; side < 2; side++) {
        piranhas_ground_copies[side] = func_80023FC8(object->model[0]);
    }
}

void widescreen_piranhas_trees(omObjData* object) {
    s32 i;
    s32 side;

    for (i = 0; i < PIRANHAS_TREES; i++) {
        unk_ovl_2D_struct* tree = &D_800F2B7C[object->model[1 + i]];

        for (side = 0; side < 2; side++) {
            unk_ovl_2D_struct* copy;

            if (piranhas_copies[i][side] < 0) {
                continue;
            }
            copy = &D_800F2B7C[piranhas_copies[i][side]];
            copy->unk_24 = tree->unk_24 + (side == 0 ? -PIRANHAS_RING : PIRANHAS_RING);
            copy->unk_28 = tree->unk_28;
            copy->unk_2C = tree->unk_2C;
            copy->unk_30 = tree->unk_30;
            copy->unk_34 = tree->unk_34;
            copy->unk_38 = tree->unk_38;
            copy->unk_3C = tree->unk_3C;
            copy->unk_40 = tree->unk_40;
            copy->unk_44 = tree->unk_44;
        }
    }
}

// Limbo Dance (overlay 0x29)'s scenery along the course, the torches, the Toads, the bushes and the trees and ferns
// behind them, is 35 props (D_800FF220_LimboDance, made in func_800FA418_LimboDance), each a form of its own, which
// the game keeps within the course's scroll plus or minus 1050: each frame (func_800FB058_LimboDance's loop) it moves
// one that has fallen behind the scroll minus 1050 on by 2100, so they repeat every 2100. The camera follows the
// scroll, and a picture wider than 4:3 reached past the ring's ends, so props popped out at its left edge and in at its
// right as the players went on. A copy of each a ring's width away, on the side away from the camera's middle, placed
// each frame where the game's own one is, carries them on as they repeat: a prop left of the camera has its copy 2100
// to its right, past the ring's right end, and one right of it its copy to its left. One copy each is enough as long
// as the picture's half width at a prop's depth is under 2100. The 5 ferns in front of the course, 400 in front of it,
// are left out: so near the camera the picture is narrower than the ring there at any width. us.toml's hooks call
// these once the setup has made the props and after the frame's loop has moved them. The 30 copies take 30 of the
// game's 128 forms, of which the minigame uses 91.
#define LIMBO_PROPS 35
#define LIMBO_RING 2100.0f
#define LIMBO_TORCH 2

typedef struct {
    /* 0x00 */ s16 kind; // 2 a torch, with its flame and two glows
    /* 0x02 */ char unk02[0x26];
    /* 0x28 */ Vec3f pos;
    /* 0x34 */ char unk34[0xC];
    /* 0x40 */ s16 model;
    /* 0x42 */ s16 sprite[3]; // a torch's flame and glows, in the minigame's sprites (below)
    /* 0x48 */ char unk48[0x18];
} LimboProp; // sizeof 0x60
extern LimboProp* D_800FF220_LimboDance;

// A torch's flame and its two glows are sprites of the minigame's own (D_80103298, a list of D_800FF1FC_LimboDance
// made in func_800FB93C_LimboDance, 16 long, the torches' 9 the only ones it uses), which the game moves to the torch
// as the torch moves on. The copies of the torches' have places of their own added to the end of the list, 9 more, so
// that the game's draw (func_800FE3CC_LimboDance) makes room for each's matrix: us.toml's hook asks for them as the
// list is made (widescreen_limbo_sprites). Each is a copy of its torch's sprite, its display lists and vertices
// included, moved as the torch's copy is, and the game draws it as its own. Until the torch's is there, and once it
// is gone, a copy is marked used and hidden (its first word not -1, LIMBO_SPRITE_HIDDEN), so the game never takes its
// place for a sprite of its own. The game frees a sprite's lists and vertices when it ends one (func_800FDE38_LimboDance:
// at the minigame's end, each), and those of a copy are its torch's, so us.toml's hook there turns a copy's away
// (widescreen_limbo_sprite_freed).
#define LIMBO_TORCHES 3
#define LIMBO_SPRITE_COPIES (LIMBO_TORCHES * 3)
#define LIMBO_SPRITE_HIDDEN 0x20
#define LIMBO_SPRITE_WORDS (0x5C / 4)

typedef struct {
    /* 0x00 */ s32 list; // -1 when unused
    /* 0x04 */ char unk04[0x24];
    /* 0x28 */ u8 flags;
    /* 0x29 */ char unk29[0xF];
    /* 0x38 */ Vec3f pos;
    /* 0x44 */ char unk44[0x18];
} LimboSprite; // sizeof 0x5C
extern LimboSprite* D_80103298;

static s16 limbo_copies[LIMBO_PROPS];
static s32 limbo_sprites; // the list's own length, the copies' after it

s32 widescreen_limbo_sprites(s32 length) {
    limbo_sprites = length & 0xFF;
    return limbo_sprites + LIMBO_SPRITE_COPIES;
}

static void limbo_sprite_hidden(LimboSprite* sprite) {
    sprite->list = 0;
    sprite->flags = LIMBO_SPRITE_HIDDEN;
}

void widescreen_limbo_sprites_made(void) {
    s32 i;

    for (i = 0; i < LIMBO_SPRITE_COPIES; i++) {
        limbo_sprite_hidden(&D_80103298[limbo_sprites + i]);
    }
}

s32 widescreen_limbo_sprite_freed(s32 index) {
    s16 sprite = index;

    if (sprite >= limbo_sprites && sprite < limbo_sprites + LIMBO_SPRITE_COPIES) {
        limbo_sprite_hidden(&D_80103298[sprite]);
        return -1;
    }
    return index;
}

void widescreen_limbo_scenery_copied(void) {
    s32 i;

    for (i = 0; i < LIMBO_PROPS; i++) {
        LimboProp* prop = &D_800FF220_LimboDance[i];

        limbo_copies[i] = -1;
        if (prop->model != 0 && prop->pos.z < 0.0f) {
            limbo_copies[i] = func_80023FC8(prop->model);
        }
    }
}

static void limbo_sprite_copied(s16 index, s32 place, f32 shift) {
    LimboSprite* copy = &D_80103298[limbo_sprites + place];
    u32* to = (u32*) copy;
    u32* from;
    s32 i;

    if (index < 0 || index >= limbo_sprites || D_80103298[index].list == -1) {
        limbo_sprite_hidden(copy);
        return;
    }
    from = (u32*) &D_80103298[index];
    for (i = 0; i < LIMBO_SPRITE_WORDS; i++) {
        to[i] = from[i];
    }
    copy->pos.x += shift;
}

void widescreen_limbo_scenery(void) {
    s32 torch = 0;
    s32 i;
    s32 k;

    for (i = 0; i < LIMBO_PROPS; i++) {
        LimboProp* prop = &D_800FF220_LimboDance[i];
        f32 shift = prop->pos.x < Center.x ? LIMBO_RING : -LIMBO_RING;
        unk_ovl_2D_struct* own;
        unk_ovl_2D_struct* copy;

        if (prop->kind == LIMBO_TORCH && torch < LIMBO_TORCHES) {
            for (k = 0; k < 3; k++) {
                if (limbo_copies[i] < 0) {
                    limbo_sprite_hidden(&D_80103298[limbo_sprites + torch * 3 + k]);
                } else {
                    limbo_sprite_copied(prop->sprite[k], torch * 3 + k, shift);
                }
            }
            torch++;
        }
        if (limbo_copies[i] < 0) {
            continue;
        }
        own = &D_800F2B7C[prop->model];
        copy = &D_800F2B7C[limbo_copies[i]];
        copy->unk_24 = own->unk_24 + shift;
        copy->unk_28 = own->unk_28;
        copy->unk_2C = own->unk_2C;
        copy->unk_30 = own->unk_30;
        copy->unk_34 = own->unk_34;
        copy->unk_38 = own->unk_38;
        copy->unk_3C = own->unk_3C;
        copy->unk_40 = own->unk_40;
        copy->unk_44 = own->unk_44;
    }
}

// Musical Mushroom (overlay 0x0B)'s floor, the grass and the sand ring the players run round on, is one model, its
// stage object's (file 0x1D0000, made in func_800F9D70_MusicalMushroom), a square about 2880 wide on the origin. The
// camera's settled view looks down past the square's left and right sides at the top of a picture wider than 4:3, and its
// opening turn round the stage past its edges, where the frame's clear showed black. Widening the floor would take the
// sand ring out from under the players and the mushrooms with it, so it keeps its size, and a copy of it lies beside
// each of its sides and corners, the grass going on at its own scale. Each copy is turned half round, so that a corner's
// meets the floor as its mirror image across both edges and a side's meets it with the floor's opposite edge, whose
// grass and shading are closer to its own than the edge a copy not turned would bring (a mirrored copy, with a scale
// below 0, is not drawn). They are a little lower than the floor, so that the game's own floor is the one drawn where
// they overlap, and each copy's sand ring is over 2200 from the middle, past what the camera sees. us.toml's hook calls
// this once the setup has made the floor; nothing moves them after. They take 8 of the game's 128 forms, of which the
// minigame uses 88.
#define MUSHROOM_FLOOR_STEP 2874.0f
#define MUSHROOM_FLOOR_BELOW 2.0f

void widescreen_musical_floor(omObjData* stage) {
    s32 x;
    s32 z;

    for (x = -1; x <= 1; x++) {
        for (z = -1; z <= 1; z++) {
            s16 copy;

            if (x == 0 && z == 0) {
                continue;
            }
            copy = func_80023FC8(stage->model[0]);
            if (copy < 0) {
                continue;
            }
            D_800F2B7C[copy].unk_24 = x * MUSHROOM_FLOOR_STEP;
            D_800F2B7C[copy].unk_28 = -MUSHROOM_FLOOR_BELOW;
            D_800F2B7C[copy].unk_2C = z * MUSHROOM_FLOOR_STEP;
            D_800F2B7C[copy].unk_34 = 180.0f; // its turn about the up axis, in degrees
        }
    }
}

// The seas and fields of short_scenes that are widened in x as they are made, so that they reach a wide window's edges
// (board_back_stage_widened, which moves the scene's widest out with them): a sea or a field with nothing standing at
// its edges, which reads the same wider. A room's walls, a stage built for 4:3 and a painted backdrop keep their cap, and
// so does Ground Pound's forest floor, whose texture showed smeared stretched that far, bare of the trees past the
// middle, Flower Shower's field, whose rocky rim around the pond showed stretched with it, and Tightrope Treachery's sea,
// which keeps its width whatever its object's or its model's scale. us.toml's hooks call these in each one's setup, once
// the stage is made; the game never writes these scales again.
static void widen_object(omObjData* object, f32 widest) {
    object->scale.x *= board_back_stage_widened(widest);
}

static void widen_model(s16 model, f32 widest) {
    D_800F2B7C[model].unk_3C *= board_back_stage_widened(widest);
}

// Hammer Drop's sea, a grid of 6x6 squares the game builds in code (func_800FA2E0_HammerDrop) from x -2400 to 2400.
void widescreen_hammer_sea(s16 model) {
    widen_model(model, 1.65f);
}

// Bumper Balls' sea, a grid built the same way (func_800FB2EC_BumperBalls) from x -1200 to 1200, the island apart.
void widescreen_bumper_sea(s16 model) {
    widen_model(model, 1.6f);
}

// Mushroom Mixup's sea, its first model (file 0x2C0003), whose texture the game scrolls.
void widescreen_mushroom_sea(omObjData* sea) {
    widen_object(sea, 1.55f);
}

// Platform Peril (overlay 0x18): a platform that has fallen away is parked by its frame function
// (func_800F8220_PlatformPeril or func_800F87A0_PlatformPeril), once it has fallen past y -5000, at (-5000, -5000,
// -5000) and still drawn, which the camera, looking down the path from above, has just past the bottom left of a 4:3
// picture. A wider one showed the parked platforms popping in and out at its bottom left as the camera swayed.
// us.toml's hooks call this at the end of both, with the platform's object: a parked platform's models hidden, as
// the game hides the players it parks once they fall. Only its models parked with it, within a few hundred units of
// that spot (its side piece sits beside it): one of the object's slots names the sky, which stays at the origin,
// and hiding every slot's model hid the sky too.
#define PLATFORM_PARKED -5000.0f
#define PLATFORM_PARKED_NEAR 1000.0f

static s32 platform_parked_near(f32 at) {
    return at > PLATFORM_PARKED - PLATFORM_PARKED_NEAR && at < PLATFORM_PARKED + PLATFORM_PARKED_NEAR;
}

void widescreen_platform_parked(omObjData* platform) {
    s32 i;

    if (platform->trans.x != PLATFORM_PARKED || platform->trans.y != PLATFORM_PARKED ||
        platform->trans.z != PLATFORM_PARKED) {
        return;
    }
    for (i = 0; i < platform->mdlcnt; i++) {
        s16 index = platform->model[i];
        unk_ovl_2D_struct* model;

        if (index < 0) {
            continue;
        }
        model = &D_800F2B7C[index];
        if (platform_parked_near(model->unk_24) && platform_parked_near(model->unk_28) &&
            platform_parked_near(model->unk_2C)) {
            func_800258EC(index, 4, 4);
        }
    }
}
