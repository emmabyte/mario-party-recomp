#include "patches.h"

// Paddle Battle (overlay 0x33): its river's water, four models one after another down the course (file 0x2089, a
// translucent surface the game draws without writing depth), which its setup (func_800FBC00_PaddleBattle) places from
// a table of heights and depths: (0, 7500), (0, 2500), (0, -2497), (-3, -7485). Their edges miss each other by a unit
// or two: the second's and third's first edges are 1 unit lower than the last edge of the one before, the fourth's 1
// unit higher, and the second's starts 2 units down the course from the first's end, the fourth's 2 units up from the
// third's. Seen from above, the two surfaces at a join lie over each other in a thin band where both blends show, a
// bright line across the river that the N64's 240 lines hid and a window's resolution shows.
//
// us.toml's hook calls this once the setup has placed the four, and moves each after the first so its first edge meets
// the last edge of the one before it, as that one has moved (measured in the running game).

// Each section's move, up the course (z) and up (y). The first stays where it is.
static const f32 river_moves[4][2] = { { 0.0f, 0.0f }, { 2.0f, 1.0f }, { 2.0f, 2.0f }, { 0.0f, 1.0f } };

// The water's models are the object's fifth to eighth.
#define RIVER_FIRST 4

void paddle_battle_river_joined(omObjData* river) {
    s32 i;

    for (i = 1; i < 4; i++) {
        unk_ovl_2D_struct* water = &D_800F2B7C[river->model[RIVER_FIRST + i]];
        water->unk_2C += river_moves[i][0];
        water->unk_28 += river_moves[i][1];
    }
}
