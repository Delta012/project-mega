#include "../include/pokemon_storage_system.h"

#include "../include/config.h"
#include "../include/constants/species.h"
#include "../include/debug.h"
#include "../include/message.h"
#include "../include/pokemon.h"
#include "../include/save.h"
#include "../include/script.h"
#include "../include/task.h"
#include "../include/types.h"

#ifdef ALLOW_SAVE_CHANGES
#ifdef EXPAND_PC_BOXES

u32 PCStorage_sizeof(void)
{
    return sizeof(PCStorage);
}

void PCStorage_InitializeBoxes(PCStorage *storage)
{
    u32 i, j;
    void *msgData;

    for (i = 0; i < NUM_PC_BOXES; i++) {
        for (j = 0; j < MONS_PER_BOX; j++) {
            BoxMonInit(&storage->boxes[i].mons[j]);
#ifdef DEBUG_INIT_PC_BOXES_WITH_MONS
            u32 species = SPECIES_VICTINI + j + i * MONS_PER_BOX; // j + 1 + i * MONS_PER_BOX + ((j + 1 + i * MONS_PER_BOX) > SPECIES_ARCEUS ? 50 : 0);
            u32 form = 0;
            if (species <= MAX_SPECIES_INCLUDING_FORMS) {
                if (species > SPECIES_MAX_MON_NUM) {
                    form = GetFormFromAdjustedForm(species);
                    species = GetBaseSpeciesFromAdjustedForm(species);
                }
                CreateBoxMonData(&storage->boxes[i].mons[j], species, 50, 31, FALSE, 0, FALSE, 0);
                if (form) {
                    SetBoxMonData(&storage->boxes[i].mons[j], MON_DATA_FORM, &form);
                }
            }
#endif // DEBUG_INIT_PC_BOXES_WITH_MONS
        }
    }

    for (i = 0, j = 0; i < NUM_PC_BOXES; i++) {
        storage->wallpapers[i] = j;
        j++;
        if (j >= DEFAULT_WALLPAPER_MAX) {
            j -= DEFAULT_WALLPAPER_MAX;
        }
    }

    storage->unlockedWallpapers = 0;

    msgData = NewMsgDataFromNarc(MSGDATA_LOAD_LAZY, 27, 24, 0);
    if (msgData != NULL) {
        for (i = 0; i < NUM_PC_BOXES; i++) {
            ReadMsgDataIntoU16Array(msgData, i + 95, storage->box_names[i]);
        }
        DestroyMsgData(msgData);
    }

    storage->curBox = 0;
}

BOOL PCStorage_PlaceMonInFirstEmptySlotInAnyBox(PCStorage *storage, struct BoxPokemon *boxMon)
{
    s32 i = storage->curBox;
    do {
        if (PCStorage_PlaceMonInBoxFirstEmptySlot(storage, i, boxMon)) {
            PCStorage_SetBoxModified(storage, i);
            return TRUE;
        }
        i++;
        if (i >= NUM_PC_BOXES) {
            i = 0;
        }
    } while (i != storage->curBox);
    return FALSE;
}

BOOL PCStorage_PlaceMonInBoxFirstEmptySlot(PCStorage *storage, u32 boxno, struct BoxPokemon *boxMon)
{
    u32 i;
    if (boxno == -1u) {
        boxno = storage->curBox;
    }
    for (i = 0; i < MONS_PER_BOX; i++) {
        if (GetBoxMonData(&storage->boxes[boxno].mons[i], MON_DATA_SPECIES, NULL) == SPECIES_NONE) {
            storage->boxes[boxno].mons[i] = *boxMon;
            PCStorage_SetBoxModified(storage, boxno);
            return TRUE;
        }
    }
    return FALSE;
}

#endif // EXPAND_PC_BOXES
#endif // ALLOW_SAVE_CHANGES

BOOL PCStorage_PlaceMonInBoxByIndexPair(PCStorage *storage, u32 boxno, u32 slotno, struct BoxPokemon *boxMon)
{
#ifdef ALLOW_SAVE_CHANGES
#ifdef EXPAND_PC_BOXES
    if (boxno == -1u) {
        boxno = storage->curBox;
    }
    if (boxno < NUM_PC_BOXES && slotno < MONS_PER_BOX) {
        storage->boxes[boxno].mons[slotno] = *boxMon;
        PCStorage_SetBoxModified(storage, boxno);
        return TRUE;
    }
    GF_ASSERT(0);
    return FALSE;
#endif
#endif
}

void PCStorage_SwapMonsInBoxByIndexPair(PCStorage *storage, u32 boxno, u32 from, u32 to)
{
#ifdef ALLOW_SAVE_CHANGES
#ifdef EXPAND_PC_BOXES
    struct BoxPokemon temp;

    temp = storage->boxes[boxno].mons[from];
    storage->boxes[boxno].mons[from] = storage->boxes[boxno].mons[to];
    storage->boxes[boxno].mons[to] = temp;
    PCStorage_SetBoxModified(storage, boxno);
#endif
#endif
}

#ifdef ALLOW_SAVE_CHANGES
#ifdef EXPAND_PC_BOXES

void PCStorage_DeleteBoxMonByIndexPair(PCStorage *storage, u32 boxno, u32 slotno)
{
    if (boxno == -1u) {
        boxno = storage->curBox;
    }
    if (slotno < MONS_PER_BOX && boxno < NUM_PC_BOXES) {
        BoxMonInit(&storage->boxes[boxno].mons[slotno]);
        PCStorage_SetBoxModified(storage, boxno);
        return;
    }

    GF_ASSERT(0);
}

int PCStorage_GetActiveBox(PCStorage *storage)
{
    return storage->curBox;
}

int PCStorage_FindFirstBoxWithEmptySlot(PCStorage *storage)
{
    int boxno = storage->curBox;
    int i;

    do {
        for (i = 0; i < MONS_PER_BOX; i++) {
            if (!GetBoxMonData(&storage->boxes[boxno].mons[i], MON_DATA_SPECIES_EXISTS, NULL)) {
                return boxno;
            }
        }
        boxno++;
        if (boxno >= NUM_PC_BOXES) {
            boxno = 0;
        }
    } while (boxno != storage->curBox);
    return NUM_PC_BOXES;
}

BOOL PCStorage_FindFirstEmptySlot(PCStorage *storage, int *boxno_p, int *slotno_p)
{
    int i, j;

    if (*boxno_p == -1) {
        *boxno_p = storage->curBox;
    }

    i = *boxno_p;
    j = *slotno_p;

    do {
        while (j < MONS_PER_BOX) {
            if (!GetBoxMonData(&storage->boxes[i].mons[j], MON_DATA_SPECIES_EXISTS, NULL)) {
                *boxno_p = i;
                *slotno_p = j;
                return TRUE;
            }
            j++;
        }
        i++;
        if (i >= NUM_PC_BOXES) {
            i = 0;
        }
        if (i == *boxno_p) {
            break;
        }
        j = 0;
    } while (1);

    // Long-standing bug: should return FALSE.
    // Has no effect on game because
    // the return value is never used.
    return NUM_PC_BOXES;
}

int PCStorage_CountEmptySpotsInAllBoxes(PCStorage *storage)
{
    int i, j, count = 0;
    for (i = 0; i < NUM_PC_BOXES; i++) {
        for (j = 0; j < MONS_PER_BOX; j++) {
            if (!GetBoxMonData(&storage->boxes[i].mons[j], MON_DATA_SPECIES_EXISTS, NULL)) {
                count++;
            }
        }
    }
    return count;
}

int PCStorage_CountEmptySpotsInBox(PCStorage *storage, u32 boxno)
{
    int i, count;

    if (boxno == -1u) {
        boxno = storage->curBox;
    }
    GF_ASSERT(boxno < NUM_PC_BOXES);
    count = 0;
    for (i = 0; i < MONS_PER_BOX; i++) {
        if (!GetBoxMonData(&storage->boxes[boxno].mons[i], MON_DATA_SPECIES_EXISTS, NULL)) {
            count++;
        }
    }
    return count;
}

void PCStorage_SetActiveBox(PCStorage *storage, u32 boxno)
{
    if (boxno < NUM_PC_BOXES) {
        storage->curBox = boxno;
        return;
    }
    GF_ASSERT(0);
}

u8 PCStorage_GetBoxWallpaper(PCStorage *storage, u32 boxno)
{
    if (boxno < NUM_PC_BOXES) {
        return storage->wallpapers[boxno];
    }
    GF_ASSERT(0);
    return 0;
}

BOOL PCStorage_IsValidWallpaperId(u8 wallpaperno)
{
    return (/*wallpaperno >= DEFAULT_WALLPAPER_MIN && */ wallpaperno < DEFAULT_WALLPAPER_MAX)
        || (wallpaperno >= BONUS_WALLPAPER_MIN && wallpaperno < BONUS_WALLPAPER_MAX);
}

void PCStorage_SetBoxWallpaper(PCStorage *storage, u32 boxno, u8 wallpaperno)
{
    if (boxno == -1u) {
        boxno = storage->curBox;
    }
    if (boxno < NUM_PC_BOXES && PCStorage_IsValidWallpaperId(wallpaperno) == TRUE) {
        storage->wallpapers[boxno] = wallpaperno;
        return;
    }
    GF_ASSERT(0);
}

void PCStorage_GetBoxName(PCStorage *storage, u32 boxno, String *dest)
{
    if (boxno == -1u) {
        boxno = storage->curBox;
    }
    if (boxno < NUM_PC_BOXES) {
        CopyU16ArrayToString(dest, storage->box_names[boxno]);
        return;
    }
    GF_ASSERT(0);
}

void PCStorage_SetBoxName(PCStorage *storage, u32 boxno, const String *src)
{
    if (boxno == -1u) {
        boxno = storage->curBox;
    }
    if (boxno < NUM_PC_BOXES) {
        CopyStringToU16Array(src, storage->box_names[boxno], BOX_NAME_LENGTH);
    }
}

int PCStorage_CountMonsAndEggsInBox(PCStorage *storage, u32 boxno)
{
    int i, count;
    if (boxno == -1u) {
        boxno = storage->curBox;
    }
    if (boxno < NUM_PC_BOXES) {
        count = 0;
        for (i = 0; i < MONS_PER_BOX; i++) {
            if (GetBoxMonData(&storage->boxes[boxno].mons[i], MON_DATA_SPECIES_EXISTS, NULL)) {
                count++;
            }
        }
        return count;
    }

    GF_ASSERT(0);
    return 0;
}

int PCStorage_CountMonsInBox(PCStorage *storage, u32 boxno)
{
    int i, count;
    if (boxno == -1u) {
        boxno = storage->curBox;
    }
    if (boxno < NUM_PC_BOXES) {
        count = 0;
        for (i = 0; i < MONS_PER_BOX; i++) {
            if (GetBoxMonData(&storage->boxes[boxno].mons[i], MON_DATA_SPECIES_EXISTS, NULL)
                && !GetBoxMonData(&storage->boxes[boxno].mons[i], MON_DATA_IS_EGG, NULL)) {
                count++;
            }
        }
        return count;
    }

    GF_ASSERT(0);
    return 0;
}

int PCStorage_CountMonsInAllBoxes(PCStorage *storage)
{
    int count;
    u32 i;

    for (i = 0, count = 0; i < NUM_PC_BOXES; i++) {
        count += PCStorage_CountMonsInBox(storage, i);
    }
    return count;
}

#endif // EXPAND_PC_BOXES
#endif // ALLOW_SAVE_CHANGES

u32 PCStorage_GetMonDataByIndexPair(PCStorage *storage, u32 boxno, u32 slotno, int attr, void *ptr)
{
#ifdef ALLOW_SAVE_CHANGES
#ifdef EXPAND_PC_BOXES
    GF_ASSERT(boxno < NUM_PC_BOXES || boxno == -1u);
    GF_ASSERT(slotno < MONS_PER_BOX);
    if (boxno == -1u) {
        boxno = storage->curBox;
    }
    return GetBoxMonData(&storage->boxes[boxno].mons[slotno], attr, ptr);
#endif
#endif
}

#ifdef ALLOW_SAVE_CHANGES
#ifdef EXPAND_PC_BOXES

struct BoxPokemon *PCStorage_GetMonByIndexPair(PCStorage *storage, u32 boxno, u32 slotno)
{
    GF_ASSERT(boxno < NUM_PC_BOXES || boxno == -1u);
    GF_ASSERT(slotno < MONS_PER_BOX);
    if (boxno == -1u) {
        boxno = storage->curBox;
    }
    return &storage->boxes[boxno].mons[slotno];
}

void PCStorage_UnlockBonusWallpaper(PCStorage *storage, u32 wallpaper)
{
    GF_ASSERT(wallpaper < NUM_BONUS_WALLPAPER);
    storage->unlockedWallpapers |= (1 << wallpaper);
}

BOOL PCStorage_IsBonusWallpaperUnlocked(PCStorage *storage, u32 wallpaper)
{
    GF_ASSERT(wallpaper < NUM_BONUS_WALLPAPER);
    return (storage->unlockedWallpapers & (1 << wallpaper)) != 0;
}

void PCStorage_SetBoxModified(PCStorage *storage, u8 boxno)
{
    if (boxno >= NUM_PC_BOXES) {
        GF_ASSERT(0);
        return;
    }
    storage->boxModifiedFlag |= 1 << boxno;
}

void PCStorage_SetAllBoxesModified(PCStorage *storage)
{
    storage->boxModifiedFlag = BOX_ALL_MODIFIED_FLAG;
}

void PCStorage_ResetBoxModifiedFlags(PCStorage *storage)
{
    storage->boxModifiedFlag = 0;
}

u32 PCStorage_GetBoxModifiedFlags(PCStorage *storage)
{
    return storage->boxModifiedFlag;
}

void sub_02074128(PCStorage *storage)
{
    u8 i, j;

    for (i = 0; i < NUM_PC_BOXES; i++) {
        for (j = 0; j < 16; j++) {
            GF_ASSERT(storage->boxes[i].unk_FF0[j] == 0);
            storage->boxes[i].unk_FF0[j] = 0;
        }
    }
}

#endif
#endif

#ifdef ALLOW_SAVE_CHANGES
#ifdef EXPAND_PC_BOXES

void PCStorage_HealAllBoxMons(PCStorage *storage)
{
    u32 i, j;

    for (i = 0; i < NUM_PC_BOXES; i++) {
        for (j = 0; j < MONS_PER_BOX; j++) {
            struct BoxPokemon *boxMon = &storage->boxes[i].mons[j];

            if (GetBoxMonData(boxMon, MON_DATA_SPECIES_EXISTS, NULL)
                && !GetBoxMonData(boxMon, MON_DATA_IS_EGG, NULL)) {
                RestoreBoxMonPP(boxMon);
                boxMon->Unused = 0;
            }
        }
        PCStorage_SetBoxModified(storage, i);
    }
}

BOOL LONG_CALL ScrCmd_HealParty_Detached(void *ctx)
{
    void *saveData = SaveBlock2_get();

    (void)ctx;
    HealParty(SaveData_GetPlayerPartyPtr(saveData));
    PCStorage_HealAllBoxMons(SaveArray_PCStorage_Get(saveData));
    return FALSE;
}

// Layouts reconstructed from the field offsets used by overlay 14
typedef struct PCUiCtx {
    u8 pad0[4];
    PCStorage *storage;     // +0x04
    struct Party *party;    // +0x08
    u8 pad1[0x13];
    u8 boxno;               // +0x1F
} PCUiCtx;

typedef struct PCUiMove {
    struct PartyPokemon *mon;  // +0x00
    u32 srcSlot;               // +0x04 (>= 0x1E means party slot)
    u32 dstSlot;               // +0x08
} PCUiMove;

struct BoxPokemon *LONG_CALL ov14_021E60C0(PCUiCtx *ctx, u8 boxno, u32 slot);
void LONG_CALL ov14_021E6100(PCUiCtx *ctx, u8 boxno, u32 slot);
void LONG_CALL ov14_021F4958(PCUiCtx *ctx, u8 boxno);
void LONG_CALL ov14_021F4A20(PCUiCtx *ctx, u8 boxno);
BOOL LONG_CALL Party_AddMon(struct Party *party, struct PartyPokemon *mon);
void LONG_CALL CopyPokemonToPokemon(struct PartyPokemon *src, struct PartyPokemon *dest);
void LONG_CALL Party_SafeCopyMonToSlot_ResetAprijuiceModifiers(struct Party *party, int idx, struct PartyPokemon *mon);

// The 13 spare header bits of BoxPokemon store: bits 0-9 = HP lost, bits 10-12 = status code
#define BOXHP_LOSS_BITS 10
#define BOXHP_LOSS_MASK 0x3FF

// Status bits as listed in the comment above PartyOnlyPokemon (pokemon.h)
#define BOXHP_ST_SLEEP_MASK 0x07
#define BOXHP_ST_POISON     (1 << 3)
#define BOXHP_ST_BURN       (1 << 4)
#define BOXHP_ST_FREEZE     (1 << 5)
#define BOXHP_ST_PARALYSIS  (1 << 6)
#define BOXHP_ST_TOXIC      (1 << 7)

static u32 BoxHp_EncodeStatus(u32 st)
{
    if (st & (BOXHP_ST_POISON | BOXHP_ST_TOXIC)) return 2;
    if (st & BOXHP_ST_BURN) return 3;
    if (st & BOXHP_ST_FREEZE) return 4;
    if (st & BOXHP_ST_PARALYSIS) return 5;
    if (st & BOXHP_ST_SLEEP_MASK) return 1;
    return 0;
}

static u32 BoxHp_DecodeStatus(u32 code)
{
    switch (code) {
    case 1: return 2; // sleep: the turn counter is lost, so a fixed one is used
    case 2: return BOXHP_ST_POISON;
    case 3: return BOXHP_ST_BURN;
    case 4: return BOXHP_ST_FREEZE;
    case 5: return BOXHP_ST_PARALYSIS;
    default: return 0;
    }
}

// BoxPokemon is the first member of the party Pokemon struct, so a plain cast is enough
static void BoxHp_Stamp(struct PartyPokemon *mon)
{
    struct BoxPokemon *box = (struct BoxPokemon *)mon;
    u32 hp = GetMonData(mon, MON_DATA_HP, NULL);
    u32 maxHp = GetMonData(mon, MON_DATA_MAXHP, NULL);
    u32 st = GetMonData(mon, MON_DATA_STATUS, NULL);
    u32 loss = (maxHp > hp) ? (maxHp - hp) : 0;

    if (loss > BOXHP_LOSS_MASK) {
        loss = BOXHP_LOSS_MASK;
    }
    box->Unused = loss | (BoxHp_EncodeStatus(st) << BOXHP_LOSS_BITS);
}

// Must run after CopyBoxPokemonToPokemon, which recalculates stats and resets HP/status
static void BoxHp_Restore(struct PartyPokemon *mon)
{
    struct BoxPokemon *box = (struct BoxPokemon *)mon;
    u32 packed = box->Unused;
    u32 loss, maxHp, st;
    u16 hp;

    if (packed == 0) {
        return; // nothing stored: leave the Pokemon fully healed
    }
    loss = packed & BOXHP_LOSS_MASK;
    maxHp = GetMonData(mon, MON_DATA_MAXHP, NULL);
    hp = (loss >= maxHp) ? 0 : (maxHp - loss);
    st = BoxHp_DecodeStatus(packed >> BOXHP_LOSS_BITS);
    SetMonData(mon, MON_DATA_HP, &hp);
    SetMonData(mon, MON_DATA_STATUS, &st);
    box->Unused = 0; // clear so the stored state can never go stale
}

// Deposit: party -> box
void LONG_CALL ov14_021E61BC_Detached(PCUiCtx *ctx, PCUiMove *mv)
{
    u8 zero = 0;
    struct PartyPokemon *mon = Party_GetMonByIndex(ctx->party, mv->srcSlot - 0x1E);

    SetMonData(mon, MON_DATA_MOOD, &zero);
    BoxHp_Stamp(mon);
    PCStorage_PlaceMonInBoxByIndexPair(ctx->storage, ctx->boxno, mv->dstSlot, (struct BoxPokemon *)mon);
    ov14_021E6100(ctx, ctx->boxno, mv->srcSlot);
    ov14_021F4958(ctx, ctx->boxno);
    ov14_021F4A20(ctx, ctx->boxno);
}

// Withdraw: box -> party
void LONG_CALL ov14_021E6184_Detached(PCUiCtx *ctx, PCUiMove *mv)
{
    struct BoxPokemon *src = ov14_021E60C0(ctx, ctx->boxno, mv->srcSlot);

    CopyBoxPokemonToPokemon(src, mv->mon);
    BoxHp_Restore(mv->mon);
    Party_AddMon(ctx->party, mv->mon);
    ov14_021E6100(ctx, ctx->boxno, mv->srcSlot);
    ov14_021F4958(ctx, ctx->boxno);
    ov14_021F4A20(ctx, ctx->boxno);
}

// Swap: party <-> box (TEMP: exact vanilla port, no HP code)
void LONG_CALL ov14_021E611C_Detached(PCUiCtx *ctx, PCUiMove *a, PCUiMove *b)
{
    u8 zero = 0;
    u32 partyIdx = b->srcSlot - 0x1E;
    struct BoxPokemon *src = ov14_021E60C0(ctx, ctx->boxno, a->srcSlot);

    CopyBoxPokemonToPokemon(src, a->mon);
    CopyPokemonToPokemon(Party_GetMonByIndex(ctx->party, partyIdx), b->mon);
    SetMonData(b->mon, MON_DATA_MOOD, &zero);
    Party_SafeCopyMonToSlot_ResetAprijuiceModifiers(ctx->party, partyIdx, a->mon);
    PCStorage_PlaceMonInBoxByIndexPair(ctx->storage, ctx->boxno, b->dstSlot, (struct BoxPokemon *)b->mon);
    ov14_021F4958(ctx, ctx->boxno);
    ov14_021F4A20(ctx, ctx->boxno);
}

#endif
#endif