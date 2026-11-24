#include "patches.h"

// Paddle Battle's (overlay 0x33) shafts of sunlight, the pale diagonal strips over the jungle. The setup makes eight
// (func_800FBAE0_PaddleBattle calls func_800FC39C_PaddleBattle eight times), each a model of file 0x120007 at a scale
// of 0.5 with two quads of its own: a faint layer of dust whose texture scrolls, and over it a pale quad, about 120 of
// its units across and 700 long, whose 16x16 IA16 texture is the shaft's light. That texture, which all eight share,
// is the colour 0xFE with an alpha of 180 - 10 x its row: the same across each row to its sides, so the strip had hard
// straight edges, starting at 70% at its top and still at 12% at its bottom, so it looked cut off in mid-air. In a wide
// window, where the shafts reach out beside the jungle, those quads read as quads.
//
// Each model's light is softened once it is made (us.toml's hook in func_800FC39C_PaddleBattle, with the model's index
// from func_800174C0): the texture fades to nothing at its sides (a smoothstep over its outer 5 columns), fades in over
// its top 3 rows and out over its bottom 4, keeping 180 - 10 x its row between, and the light's quad is half as wide
// again, so that with its outer columns faded the shaft still reads about as wide as the old strip.
// Each model has vertex buffers of its own (up to 8, its unk_08, which the dust's scrolling steps through), the light's
// four vertices the same in each, and the game never writes their x again. The light is found through the model's
// display lists, not at a fixed address, and changed only while it is the texture above: the first shaft's model finds
// it so, and the seven after it find it changed and widen their quads alone.

#define LIGHT_SIZE 16
#define LIGHT_COLOR 0xFE
#define LIGHT_WIDER 1.5f
// The display list commands that find the light: its G_SETTIMG (IA, 16-bit), its tile's size (16x16) and the four
// vertices it is drawn with.
#define CMD_SETTIMG_IA16 0xFD700000u
#define CMD_SETTILESIZE 0xF2000000u
#define LIGHT_TILE_SIZE 0x0003C03Cu
#define CMD_VTX 0x01u
#define CMD_ENDDL 0xDFu
// The light's model has two display lists, the dust's and the light's, each about 24 commands long.
#define SHAFT_LISTS 2
#define LIST_LONGEST 64

static f32 smooth(f32 t) {
    if (t <= 0.0f) {
        return 0.0f;
    }
    if (t >= 1.0f) {
        return 1.0f;
    }
    return t * t * (3.0f - 2.0f * t);
}

static f32 at_most_one(f32 t) {
    return t < 1.0f ? t : 1.0f;
}

static s32 light_unchanged(u16* light) {
    s32 x;
    s32 y;

    for (y = 0; y < LIGHT_SIZE; y++) {
        for (x = 0; x < LIGHT_SIZE; x++) {
            if (light[y * LIGHT_SIZE + x] != ((LIGHT_COLOR << 8) | (180 - 10 * y))) {
                return FALSE;
            }
        }
    }
    return TRUE;
}

static void light_soften(u16* light) {
    s32 x;
    s32 y;

    for (y = 0; y < LIGHT_SIZE; y++) {
        f32 row = (180 - 10 * y) * at_most_one(y / 3.0f) * at_most_one((LIGHT_SIZE - 1 - y) / 4.0f);

        for (x = 0; x < LIGHT_SIZE; x++) {
            f32 alpha = row * smooth(x / 5.0f) * smooth((LIGHT_SIZE - 1 - x) / 5.0f);

            light[y * LIGHT_SIZE + x] = (LIGHT_COLOR << 8) | (u8)(s32)(alpha + 0.5f);
        }
    }
}

// The light's vertices wider in each of the model's buffers, once each: a model repeats its last buffer in the slots
// it doesn't use.
static void quad_widen(unk2C0C0StructC0* data, s32 first, s32 count) {
    s32 i;
    s32 j;
    s32 v;

    for (i = 0; i < ARRAY_COUNT(data->unk_08); i++) {
        unk2C0C0StructE0* buffer = data->unk_08[i];

        if (buffer == NULL) {
            continue;
        }
        for (j = 0; j < i; j++) {
            if (data->unk_08[j] == buffer) {
                break;
            }
        }
        if (j < i) {
            continue;
        }
        for (v = first; v < first + count; v++) {
            buffer[v].unk_00 = (s16)(s32)(buffer[v].unk_00 * LIGHT_WIDER);
        }
    }
}

void paddle_battle_shaft(s16 model) {
    unk2C0C0StructC0* data = D_800F2B7C[model].unk_6C;
    s32 i;
    s32 c;

    if (data == NULL || data->unk_00 == NULL) {
        return;
    }
    for (i = 0; i < SHAFT_LISTS; i++) {
        u32* list = (u32*)data->unk_00[i];
        u16* light = NULL;
        s32 sized = FALSE;

        if (((u32)list >> 24) != 0x80) {
            continue;
        }
        for (c = 0; c < LIST_LONGEST && (list[c * 2] >> 24) != CMD_ENDDL; c++) {
            u32 w0 = list[c * 2];
            u32 w1 = list[c * 2 + 1];

            if (w0 == CMD_SETTIMG_IA16 && (w1 >> 24) == 0x80) {
                light = (u16*)w1;
            } else if (w0 == CMD_SETTILESIZE && w1 == LIGHT_TILE_SIZE) {
                sized = TRUE;
            } else if ((w0 >> 24) == CMD_VTX && light != NULL && sized) {
                // F3DEX2's G_VTX: the count in bits 12 to 19, the first index plus the count in bits 1 to 7, the
                // vertices from segment 1 (the model's buffer) at the offset in w1's low 24 bits.
                s32 count = (w0 >> 12) & 0xFF;
                s32 first = (w1 & 0xFFFFFF) / sizeof(unk2C0C0StructE0);

                if (light_unchanged(light)) {
                    light_soften(light);
                }
                quad_widen(data, first, count);
                return;
            }
        }
    }
}
