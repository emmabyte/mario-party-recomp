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
//
// A picture too big for the texture memory is made of several textures side by side, a part's batches each drawn with
// one, their quads meeting at its edges: Board Selection's picture of each board, 128x64, is four 64x32 CI8 textures,
// each with a palette of its own, quarters of one part meeting across its middle and down it. Each clamped at its own
// edges, the bilinear filter blends nothing across a join, and drawn above the console's resolution, where the filter
// smooths everything else, the join shows as a hard line across the picture.
//
// So where a batch's vertex lies where one of another texture of the part does, each at its texture's opposite edges
// (one's t at its last row's far edge, the other's at 0, their s the same, or the same across), the two textures meet
// there, and along the span such vertices share, each one's texels at the join take the colour halfway between theirs
// and the other's: the nearest in its own palette, for a CI8 texture with an RGBA16 one. The two meet at one colour, and
// the filter's step across the join is gone. Joined again (the picture made again from the same textures), the colours
// already halfway stay about where they are.

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

    constexpr uint32_t G_SETTIMG = 0xFD;
    constexpr uint32_t G_LOADTLUT = 0xF0;
    constexpr uint32_t G_SETOTHERMODE_H_TEXTLUT = 0xE3001001;   // its TLUT's format (G_MDSFT_TEXTLUT, 2 bits)
    constexpr uint32_t G_TT_RGBA16 = 0x8000;
    constexpr uint32_t G_IM_FMT_CI = 2;
    constexpr uint32_t G_IM_SIZ_8b = 1;

    // The CI8 texture the part's batches draw with now (0 if it is another kind) and its palette, and where they were
    // loaded: a batch that draws with the texture before it loads nothing.
    struct Picture {
        gpr texture = 0, palette = 0;
        int32_t width = 0, height = 0;      // texels, a row width bytes
    } picture;
    gpr picture_palette = 0;                // the last palette loaded
    bool picture_rgba16 = false;            // the TLUT's format the part draws with

    // The part's batches drawn with a CI8 texture so far: each texture and its batch's vertices.
    struct Piece {
        Picture picture;
        int32_t first = 0, end = 0;
    };
    constexpr int MAX_PIECES = 16;
    Piece pieces[MAX_PIECES];
    int piece_count = 0;
    gpr pieces_vertices = 0;

    // Two textures' joins evened already in the part, so a texture drawn in several batches is evened once.
    struct Join {
        gpr a = 0, b = 0;
        bool across = false;
    };
    Join joins[MAX_PIECES * 2];
    int join_count = 0;

    // Whether a texture or palette of n bytes lies in RDRAM (a KSEG0 address, not a segment one).
    bool in_rdram(gpr address, int32_t n) {
        uint32_t a = (uint32_t)address;
        return a >= 0x80000000u && a + (uint32_t)n <= 0x80800000u;
    }

    void rgba16_split(uint32_t c, int32_t& r, int32_t& g, int32_t& b) {
        r = (c >> 11) & 31;
        g = (c >> 6) & 31;
        b = (c >> 1) & 31;
    }

    // The palette's entry nearest r, g and b with the alpha bit given, or -1 if it has none with that alpha.
    int32_t nearest(uint8_t* rdram, gpr palette, int32_t r, int32_t g, int32_t b, uint32_t alpha) {
        int32_t best = -1, best_distance = 0;
        for (int32_t i = 0; i < 256; i++) {
            uint32_t c = MEM_HU(i * 2, palette);
            if ((c & 1) != alpha) {
                continue;
            }
            int32_t cr, cg, cb;
            rgba16_split(c, cr, cg, cb);
            int32_t distance = (cr - r) * (cr - r) + (cg - g) * (cg - g) + (cb - b) * (cb - b);
            if (best < 0 || distance < best_distance) {
                best = i;
                best_distance = distance;
            }
        }
        return best;
    }

    // Texels a and b (byte offsets in their textures) take the colour halfway between theirs, each the nearest in its
    // own palette, each keeping its alpha bit.
    void even(uint8_t* rdram, const Picture& pa, int32_t a, const Picture& pb, int32_t b) {
        uint32_t ca = MEM_HU(MEM_BU(a, pa.texture) * 2, pa.palette);
        uint32_t cb = MEM_HU(MEM_BU(b, pb.texture) * 2, pb.palette);
        int32_t ar, ag, ab, br, bg, bb;
        rgba16_split(ca, ar, ag, ab);
        rgba16_split(cb, br, bg, bb);
        int32_t r = (ar + br + 1) / 2, g = (ag + bg + 1) / 2, bl = (ab + bb + 1) / 2;
        int32_t ia = nearest(rdram, pa.palette, r, g, bl, ca & 1);
        int32_t ib = nearest(rdram, pb.palette, r, g, bl, cb & 1);
        if (ia >= 0) {
            MEM_B(a, pa.texture) = (int8_t)ia;
        }
        if (ib >= 0) {
            MEM_B(b, pb.texture) = (int8_t)ib;
        }
    }

    // Where piece p's vertices lie on q's at their opposite edges, the texels either side of that join evened, along
    // the span the shared vertices cover. Down (across false): p's last row on q's first, or q's last on p's first;
    // across: the same for columns.
    void join(uint8_t* rdram, gpr vertices, const Piece& p, const Piece& q, bool across) {
        for (int i = 0; i < join_count; i++) {
            const Join& j = joins[i];
            if (j.across == across && ((j.a == p.picture.texture && j.b == q.picture.texture) ||
                                       (j.a == q.picture.texture && j.b == p.picture.texture))) {
                return;
            }
        }
        // Which of the two is first (above, or to the left), and the span along the join (10.5), from either's
        // shared vertices.
        const Piece* first = nullptr;
        int32_t low = 0, high = 0, shared = 0;
        for (int32_t i = p.first; i < p.end; i++) {
            gpr vi = vertices + i * 16;
            for (int32_t k = q.first; k < q.end; k++) {
                gpr vk = vertices + k * 16;
                if (MEM_H(0, vi) != MEM_H(0, vk) || MEM_H(2, vi) != MEM_H(2, vk) || MEM_H(4, vi) != MEM_H(4, vk)) {
                    continue;
                }
                // The coordinate along the join (s down, t across) and the one through it.
                int32_t along_p = MEM_H(across ? 10 : 8, vi), along_q = MEM_H(across ? 10 : 8, vk);
                int32_t through_p = MEM_H(across ? 8 : 10, vi), through_q = MEM_H(across ? 8 : 10, vk);
                int32_t size_p = (across ? p.picture.width : p.picture.height) * 32;
                int32_t size_q = (across ? q.picture.width : q.picture.height) * 32;
                const Piece* here;
                if (through_p == size_p && through_q == 0) {
                    here = &p;
                } else if (through_q == size_q && through_p == 0) {
                    here = &q;
                } else {
                    continue;
                }
                if (along_p != along_q || (first != nullptr && first != here)) {
                    continue;
                }
                first = here;
                low = (shared == 0) ? along_p : std::min(low, along_p);
                high = (shared == 0) ? along_p : std::max(high, along_p);
                shared++;
            }
        }
        if (first == nullptr || high <= low) {
            return;
        }
        if (join_count < (int)(sizeof(joins) / sizeof(joins[0]))) {
            joins[join_count++] = { p.picture.texture, q.picture.texture, across };
        }

        const Picture& a = first->picture;
        const Picture& b = (first == &p) ? q.picture : p.picture;
        int32_t length = std::min(across ? a.height : a.width, across ? b.height : b.width);
        int32_t from = std::max(low >> 5, 0), to = std::min((high + 31) >> 5, length);
        for (int32_t x = from; x < to; x++) {
            if (across) {
                even(rdram, a, x * a.width + a.width - 1, b, x * b.width);
            } else {
                even(rdram, a, (a.height - 1) * a.width + x, b, x);
            }
        }
    }

    // A batch's commands read for its CI8 texture: the palette it loads, its TLUT's format and its render tile.
    void read_picture(uint8_t* rdram, gpr start, gpr end, gpr settile, gpr settilesize) {
        gpr image = 0;
        for (gpr cmd = start; cmd < end; cmd += 8) {
            uint32_t w0 = (uint32_t)MEM_W(0, cmd), w1 = (uint32_t)MEM_W(4, cmd);
            if ((w0 >> 24) == G_SETTIMG) {
                image = (gpr)(int32_t)w1;
            } else if ((w0 >> 24) == G_LOADTLUT) {
                picture_palette = image;
            } else if (w0 == G_SETOTHERMODE_H_TEXTLUT) {
                picture_rgba16 = (w1 == G_TT_RGBA16);
            }
        }
        if (settilesize == 0) {
            return;
        }
        picture = Picture();
        if (settile == 0 || settile > settilesize) {
            return;
        }
        uint32_t t0 = (uint32_t)MEM_W(0, settile), t1 = (uint32_t)MEM_W(4, settile);
        uint32_t z0 = (uint32_t)MEM_W(0, settilesize), z1 = (uint32_t)MEM_W(4, settilesize);
        int32_t width = (int32_t)(((z1 >> 12) & 0xFFF) >> 2) + 1, height = (int32_t)((z1 & 0xFFF) >> 2) + 1;
        int32_t line = (int32_t)((t0 >> 9) & 0x1FF) * 8;
        bool ci8 = ((t0 >> 21) & 7) == G_IM_FMT_CI && ((t0 >> 19) & 3) == G_IM_SIZ_8b && ((t1 >> 20) & 0xF) == 0;
        if (!ci8 || !picture_rgba16 || picture_palette == 0 || (z0 & 0x00FFFFFF) != 0 || line != width) {
            return;
        }
        picture.texture = (gpr)(int32_t)MEM_W(0, TEXTURE);
        picture.palette = picture_palette;
        picture.width = width;
        picture.height = height;
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

    // The part's CI8 pictures: this batch's checked against those before it in the part, for joins.
    {
        gpr vs = (gpr)(int32_t)MEM_W(0, VERTICES);
        int32_t first = MEM_H(0, BATCH_FIRST), last = MEM_H(0, BATCH_END);
        if (first == 0 || vs != pieces_vertices) {
            piece_count = 0;
            join_count = 0;
            pieces_vertices = vs;
            if (settilesize == 0) {
                picture = Picture();
            }
        }
        read_picture(rdram, batch_start, end, settile, settilesize);
        if (picture.texture != 0 && in_rdram(picture.texture, picture.width * picture.height) &&
            in_rdram(picture.palette, 512) && last > first && piece_count < MAX_PIECES) {
            Piece piece{ picture, first, last };
            for (int i = 0; i < piece_count; i++) {
                if (pieces[i].picture.texture != piece.picture.texture) {
                    join(rdram, vs, piece, pieces[i], false);
                    join(rdram, vs, piece, pieces[i], true);
                }
            }
            pieces[piece_count++] = piece;
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
