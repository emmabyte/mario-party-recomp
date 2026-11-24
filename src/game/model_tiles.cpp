// A 3D model's texture clamped where its triangles' part of it ends (us.toml's hooks in func_8002E488 say where).
//
// The model builder (src/2C0C0.c's func_8002E488) writes a batch of a part's triangles after the texture they are
// drawn with, its tile clamped at the texture's edges. Some models' textures are sheets of pictures stacked one under
// another, a triangle's coordinates on one of them: Crazy Cutter's cones, barricades and poles are quads on the top
// 32 rows of a 32x96 sheet, their coordinates t 0 to 32 (the middle cone's to 31.88). A picture's last rows are clear,
// but at a quad's bottom edge the bilinear filter mixes in row 32, the top row of the picture under it, and drawn above
// the console's resolution that row shows as a thin dark line under the object, a little under the bottom of what is
// drawn. Players don't see it on the console.
//
// So where the batches drawn with a texture reach no further than an edge between two texels inside it (or an eighth
// of a texel short of one), its tile is clamped at that edge, and the filter mixes in nothing past it. A batch whose
// coordinates end further between two texels' edges (most of a character's parts on its texture) keeps the texture's
// own tile, and so does one that runs to the texture's edge. Only a tile's right and bottom are clamped: its top and
// left are where the texture's memory starts.

#include "recomp.h"
#include "librecomp/helpers.hpp"

namespace {
    // The builder's globals (src/2C0C0.c).
    constexpr gpr DL_HEAD = (int32_t)0x800F37DC;        // D_800F37DC, the Gfx* it writes at
    constexpr gpr VERTICES = (int32_t)0x800F50BC;       // D_800F50BC, the part's Vtx
    constexpr gpr BATCH_FIRST = (int32_t)0x800EE32C;    // D_800EE32C, s16: the batch's first vertex
    constexpr gpr BATCH_END = (int32_t)0x800EE992;      // D_800EE992, s16: one past its last
    constexpr gpr TEXTURE = (int32_t)0x800F328C;        // D_800F328C, the texture the tile holds, NULL at a part's start

    constexpr uint32_t G_SETTILE = 0xF5;
    constexpr uint32_t G_SETTILESIZE = 0xF2;
    constexpr int G_TX_RENDERTILE = 0;
    constexpr int G_TX_CLAMP = 2;

    // How far short of a texel's edge, and past it, a coordinate (10.5) may end and still be on it: an eighth of a
    // texel short (Crazy Cutter's middle cone's quad ends at t 31.88), a sixteenth past.
    constexpr int32_t SHORT_OF_EDGE = 4;
    constexpr int32_t PAST_EDGE = 2;

    gpr batch_start = 0;        // the DL head when the batch started

    // The tile the part's stream draws with now: its SetTileSize command, 0 if none to clamp.
    struct Tile {
        gpr size = 0;
        gpr texture = 0;
        int32_t last_s = 0, last_t = 0;     // its own last texel, the texture's
        bool clamp_s = false, clamp_t = false;
        int32_t reach_s = 0, reach_t = 0;   // the furthest its batches' coordinates go (10.5)
        bool any = false;
    } tile;

    // The last texel to clamp an axis at: the edge the batches reach, or the tile's own last texel.
    int32_t clamped_last(bool clamp, int32_t reach, int32_t last) {
        if (!clamp) {
            return last;
        }
        int32_t edge = (reach + 16) >> 5;
        if (edge < 1 || edge - 1 >= last || reach - edge * 32 > PAST_EDGE || edge * 32 - reach > SHORT_OF_EDGE) {
            return last;
        }
        return edge - 1;
    }
}

// At func_8002E488's start: where the batch's commands start, and whether the tile tracked is still the stream's.
extern "C" void mp_model_batch_start(uint8_t* rdram) {
    batch_start = (gpr)(int32_t)MEM_W(0, DL_HEAD);
    if (tile.size != 0 && (gpr)(int32_t)MEM_W(0, TEXTURE) != tile.texture) {
        tile.size = 0;
    }
}

// At its end, the batch written: a texture it loaded is tracked, and the tile it draws with clamped to the edges
// its batches reach.
extern "C" void mp_model_batch_end(uint8_t* rdram) {
    gpr end = (gpr)(int32_t)MEM_W(0, DL_HEAD);
    gpr settile = 0, settilesize = 0;
    for (gpr cmd = batch_start; cmd < end; cmd += 8) {
        uint32_t w0 = (uint32_t)MEM_W(0, cmd), w1 = (uint32_t)MEM_W(4, cmd);
        if ((w0 >> 24) == G_SETTILE && ((w1 >> 24) & 7) == G_TX_RENDERTILE) {
            settile = cmd;
        } else if ((w0 >> 24) == G_SETTILESIZE && ((w1 >> 24) & 7) == G_TX_RENDERTILE) {
            settilesize = cmd;
        }
    }

    if (settilesize != 0) {
        tile = Tile();
        uint32_t w0 = (uint32_t)MEM_W(0, settilesize), w1 = (uint32_t)MEM_W(4, settilesize);
        if (settile != 0 && settile < settilesize && (w0 & 0x00FFFFFF) == 0) {
            uint32_t t1 = (uint32_t)MEM_W(4, settile);
            tile.size = settilesize;
            tile.texture = (gpr)(int32_t)MEM_W(0, TEXTURE);
            tile.last_s = ((w1 >> 12) & 0xFFF) >> 2;
            tile.last_t = (w1 & 0xFFF) >> 2;
            // cms, shifts and cmt, shiftt: a shift scales the coordinates, which then aren't the texture's texels.
            tile.clamp_s = (((t1 >> 8) & 3) & G_TX_CLAMP) && (t1 & 0xF) == 0;
            tile.clamp_t = (((t1 >> 18) & 3) & G_TX_CLAMP) && ((t1 >> 10) & 0xF) == 0;
        }
    }
    if (tile.size == 0) {
        return;
    }

    gpr vertices = (gpr)(int32_t)MEM_W(0, VERTICES);
    int32_t first = MEM_H(0, BATCH_FIRST), last = MEM_H(0, BATCH_END);
    for (int32_t i = first; i < last; i++) {
        int32_t s = MEM_H(8, vertices + i * 16), t = MEM_H(10, vertices + i * 16);
        if (!tile.any || s > tile.reach_s) {
            tile.reach_s = s;
        }
        if (!tile.any || t > tile.reach_t) {
            tile.reach_t = t;
        }
        tile.any = true;
    }
    if (!tile.any) {
        return;
    }

    int32_t last_s = clamped_last(tile.clamp_s, tile.reach_s, tile.last_s);
    int32_t last_t = clamped_last(tile.clamp_t, tile.reach_t, tile.last_t);
    uint32_t w1 = (uint32_t)MEM_W(4, tile.size);
    MEM_W(4, tile.size) = (int32_t)((w1 & 0xFF000000) | ((uint32_t)(last_s << 2) << 12) | (uint32_t)(last_t << 2));
}
