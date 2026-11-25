// The Unlocks option (src/game/config.cpp): with All, everything the game has a player earn is open: TODO. The save
// keeps what the player has earned, so back on Original the game is as they left it.
//
// The game keeps what is earned in GwCommon (common_structs.h), the first part of its save, and reads it there from
// code that is still the decomp's asm (the Mini-Game House, the board select, the shop), so the option sets those bits
// in GwCommon itself, and keeps the player's own (`earned`) beside them:
// - Every save to the EEPROM goes through WriteEeprom, which writes the player's own bits in place of the forced ones.
// - Every load from it goes through ReadEeprom, after which GwCommon holds the player's own, and the reset to a new
//   save's (func_80059280) the same.
// - While the option sets them, the game's setters of those bits set (or clear) the player's own too: a mini-game seen
//   or bought, a board's or a feature's flag.
// - The option is taken each time the game loads an overlay (OvlLoad, overlays.c): a menu, a board, Mushroom Village,
//   so a change to it is seen at the next screen.

#include "patches.h"
#include "engine/siman.h"

s32 recomp_unlock_all(void);

s32 RequestSIFunction(unkMesg* mesg, void* func, void* arg, s32 type);
s32 _WriteEeprom(void* arg);
s32 _ReadEeprom(void* arg);

typedef struct UnkEep {
    /* 0x00 */ u16 dest;
    /* 0x04 */ u8* src;
    /* 0x08 */ u16 size;
} UnkEep;

void func_80059354(s16 flag, s16* byteIndex, s16* bitIndex);
void func_80059768(s16 index, s16 param_2);
u16* func_80059520(s16 index);
void func_8005963C(s16 index, u16 param_2);

// The bits the option sets, as a GwCommon: a 1 where it sets one.
static GWCOMMON forced;
// The player's own GwCommon, of which only the bits in `forced` are used: the rest is GwCommon's own.
static GWCOMMON earned;
// Whether GwCommon holds the forced bits now.
static s32 forcing = 0;

static void forced_init(void) {
    static s32 done = 0;
    if (done) {
        return;
    }
    done = 1;
    // TODO: the set.
}

static u8* bytes(GWCOMMON* common) {
    return (u8*)common;
}

// GwCommon's bits in `forced` set, or put back to the player's own.
static void force(void) {
    s32 i;
    forced_init();
    for (i = 0; i < (s32)sizeof(GWCOMMON); i++) {
        // Until now GwCommon was the player's own, whatever the game wrote it with.
        bytes(&earned)[i] = bytes(&GwCommon)[i];
        bytes(&GwCommon)[i] |= bytes(&forced)[i];
    }
    forcing = 1;
}

static void unforce(void) {
    s32 i;
    for (i = 0; i < (s32)sizeof(GWCOMMON); i++) {
        bytes(&GwCommon)[i] = (bytes(&GwCommon)[i] & ~bytes(&forced)[i]) | (bytes(&earned)[i] & bytes(&forced)[i]);
    }
    forcing = 0;
}

// GwCommon's bytes from `start` for `size` are the player's own (loaded, or a new save's): taken as earned, and the
// forced bits set on them again if the option is setting them.
static void unlock_all_took(u8* start, s32 size) {
    s32 i;
    for (i = 0; i < size; i++) {
        s32 at = start + i - bytes(&GwCommon);
        if (at >= 0 && at < (s32)sizeof(GWCOMMON)) {
            bytes(&earned)[at] = bytes(&GwCommon)[at];
            if (forcing) {
                bytes(&GwCommon)[at] |= bytes(&forced)[at];
            }
        }
    }
}

// The option, taken (overlays.c's OvlLoad).
void unlock_all_overlay(void) {
    s32 all = recomp_unlock_all();
    if (all && !forcing) {
        force();
    } else if (!all && forcing) {
        unforce();
    }
}

// src/eeprom.c's, writing the player's own bits in place of the forced ones from a copy, when it writes GwCommon.
RECOMP_PATCH void WriteEeprom(s32 arg0, void* arg1, s16 arg2) {
    static u8 copy[sizeof(GWCOMMON)];
    unkMesg sp10;
    UnkEep sp20;

    // @recomp The player's own bits.
    if (forcing && (u8*)arg1 >= bytes(&GwCommon) && (u8*)arg1 + arg2 <= bytes(&GwCommon) + sizeof(GWCOMMON)) {
        s32 i;
        s32 from = (u8*)arg1 - bytes(&GwCommon);
        for (i = 0; i < arg2; i++) {
            s32 at = from + i;
            copy[i] = (bytes(&GwCommon)[at] & ~bytes(&forced)[at]) | (bytes(&earned)[at] & bytes(&forced)[at]);
        }
        arg1 = copy;
    }

    sp20.dest = arg0 + 8;
    sp20.src = arg1;
    sp20.size = arg2;

    RequestSIFunction(&sp10, (void*)_WriteEeprom, &sp20, 1);
}

// src/eeprom.c's.
RECOMP_PATCH void ReadEeprom(s32 eepromAbsAddr, u8* src, s16 size) {
    unkMesg sp10;
    UnkEep sp20;

    sp20.dest = eepromAbsAddr + 8;
    sp20.src = src;
    sp20.size = size;

    RequestSIFunction(&sp10, (void*)_ReadEeprom, (void*)&sp20, 1);

    // @recomp What it read into GwCommon is the player's own.
    unlock_all_took(src, size);
}

// src/59E80.c's: a new save's GwCommon (and GwSystem's and GwQuest's).
RECOMP_PATCH void func_80059280(void) {
    s32 i;
    GWCOMMON* common;

    common = &GwCommon;
    bzero(common, sizeof(GWCOMMON));
    common->unk0 = 0x12;
    common->coinNum = 0;
    common->starNum = 0;
    common->unk_46 = -1;
    common->boardItem = 0;
    SetBoardFeatureFlag(0x10);
    for (i = 0; i < 8; i++) {
        func_80059768(i, 0);
        if (i < 6) {
            func_8005963C(i, 0x8000);
        }
    }

    GwSystem.curBoardIndex = 0;
    GwSystem.unk_1E = 0; //?
    bzero(&GwQuest, sizeof(GwQuest));

    // @recomp The new save's is the player's own.
    unlock_all_took(bytes(&GwCommon), sizeof(GWCOMMON));
}

// src/59E80.c's setters, which set (or clear) the player's own bit too while the option sets them.
RECOMP_PATCH void func_800593AC(s16 arg0) {
    s16 sp10;
    s16 sp12;

    func_80059354(arg0, &sp10, &sp12);
    GwCommon.mgUnlock[sp10] = GwCommon.mgUnlock[sp10] | (1 << sp12);
    // @recomp
    earned.mgUnlock[sp10] |= 1 << sp12;
}

RECOMP_PATCH void func_80059448(s16 arg0) {
    s16 var0;
    s16 var1;

    func_80059354(arg0, &var0, &var1);
    GwCommon.mgBuy[var0] = (GwCommon.mgBuy[var0] | (1 << var1));
    // @recomp
    earned.mgBuy[var0] |= 1 << var1;
}

RECOMP_PATCH void SetBoardFeatureFlag(s32 flag) {
    GwCommon.flag[flag / 8] |= (1 << flag % 8);
    // @recomp
    earned.flag[flag / 8] |= (1 << flag % 8);
}

RECOMP_PATCH void ClearBoardFeatureFlag(s32 flag) {
    GwCommon.flag[flag / 8] &= ~(1 << flag % 8);
    // @recomp A forced flag stays set while the option sets it.
    earned.flag[flag / 8] &= ~(1 << flag % 8);
    if (forcing) {
        GwCommon.flag[flag / 8] |= forced.flag[flag / 8];
    }
}
