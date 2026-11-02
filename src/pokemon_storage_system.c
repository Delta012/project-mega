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

// Boxed Pokemon keep their HP and status.

static BOOL BoxMon_BytesEqual(const struct BoxPokemon *a, const struct BoxPokemon *b)
{
    const u32 *pa = (const u32 *)a;
    const u32 *pb = (const u32 *)b;
    u32 i;

    for (i = 0; i < sizeof(struct BoxPokemon) / sizeof(u32); i++) {
        if (pa[i] != pb[i]) {
            return FALSE;
        }
    }
    return TRUE;
}

// Restores PP and clears the stored HP/status of every boxed Pokemon.
// A box is only flagged as modified if one of its Pokemon actually changed,
// so a nurse visit doesn't force the save to rewrite all the boxes.
static void PCStorage_HealAllBoxMons(PCStorage *storage)
{
    u32 i, j;

    for (i = 0; i < NUM_PC_BOXES; i++) {
        BOOL changed = FALSE;

        for (j = 0; j < MONS_PER_BOX; j++) {
            struct BoxPokemon *boxMon = &storage->boxes[i].mons[j];

            if (GetBoxMonData(boxMon, MON_DATA_SPECIES_EXISTS, NULL)
                && !GetBoxMonData(boxMon, MON_DATA_IS_EGG, NULL)) {
                struct BoxPokemon before = *boxMon;

                RestoreBoxMonPP(boxMon);
                boxMon->Unused = 0;
                if (!BoxMon_BytesEqual(&before, boxMon)) {
                    changed = TRUE;
                }
            }
        }
        if (changed) {
            PCStorage_SetBoxModified(storage, i);
        }
    }
}

BOOL LONG_CALL PM_ScrCmd_HealParty(void *ctx)
{
    void *saveData = SaveBlock2_get();

    (void)ctx;
    HealParty(SaveData_GetPlayerPartyPtr(saveData));
    PCStorage_HealAllBoxMons(SaveArray_PCStorage_Get(saveData));
    return FALSE;
}

// ============================================================================================================
// Overlay 14 (PC app): layouts reconstructed from the field offsets it uses
// ============================================================================================================

typedef struct PCUiCtx {
    u8 pad0[4];
    PCStorage *storage;     // +0x04
    struct Party *party;    // +0x08
    u8 pad1[0x13];
    u8 boxno;               // +0x1F
} PCUiCtx;

typedef struct PCUiMove {
    struct PartyPokemon *mon;  // +0x00
    u32 srcSlot;               // +0x04
    u32 dstSlot;               // +0x08
} PCUiMove;

#define PC_UI_PARTY_SLOT_BASE 0x1E // slots >= this value are party slots: party index = slot - 0x1E
#define PC_UI_TAB_FLAG        0x80 // set in the destination when a Pokemon is dropped on a box tab

#define PC_UI_STATIC_ASSERT(name, cond) typedef char name[(cond) ? 1 : -1]
PC_UI_STATIC_ASSERT(PCUiCtxBoxnoOffset, __builtin_offsetof(PCUiCtx, boxno) == 0x1F);
PC_UI_STATIC_ASSERT(PCUiMoveSize, sizeof(PCUiMove) == 12);
PC_UI_STATIC_ASSERT(MonDataHeldItemId, MON_DATA_HELD_ITEM == 6);
PC_UI_STATIC_ASSERT(MonDataStatusId, MON_DATA_STATUS == 160);
PC_UI_STATIC_ASSERT(MonDataCapsuleId, MON_DATA_CAPSULE == 162);
PC_UI_STATIC_ASSERT(MonDataHpId, MON_DATA_HP == 163);
PC_UI_STATIC_ASSERT(MonDataMaxHpId, MON_DATA_MAXHP == 164);
PC_UI_STATIC_ASSERT(MonDataMailId, MON_DATA_MAIL_STRUCT == 170);
PC_UI_STATIC_ASSERT(MonDataSealsId, MON_DATA_SEAL_COORDS == 171);
PC_UI_STATIC_ASSERT(CapsuleSize, sizeof(CAPSULE) == 24);
PC_UI_STATIC_ASSERT(BoxPokemonSize, sizeof(struct BoxPokemon) == 136);

struct BoxPokemon *LONG_CALL ov14_021E60C0(PCUiCtx *ctx, u8 boxno, u32 slot);               // box Pokemon at a slot
u32 LONG_CALL ov14_021E6070(PCUiCtx *ctx, u32 slot, int attr, void *ptr);                    // reads one attribute of a UI slot
u32 LONG_CALL ov14_021E6464(PCUiCtx *ctx, u32 slot);                                         // box number of a tab
BOOL LONG_CALL ov14_021E6480(PCUiCtx *ctx, u32 partyIdx);                                    // TRUE if the party keeps another able Pokemon
void LONG_CALL ov14_021E6100(PCUiCtx *ctx, u8 boxno, u32 slot);                              // removes the Pokemon at a slot
void LONG_CALL ov14_021F4958(PCUiCtx *ctx, u8 boxno);                                        // redraws the box
void LONG_CALL ov14_021F4A20(PCUiCtx *ctx, u8 boxno);
u32 LONG_CALL Func_02078068(u32 item);                                                       // 1 = mail item
struct Mail *LONG_CALL Mail_New(u32 heapId);
void LONG_CALL CopyPokemonToPokemon(struct PartyPokemon *src, struct PartyPokemon *dest);
void LONG_CALL Party_SafeCopyMonToSlot_ResetAprijuiceModifiers(struct Party *party, int idx, struct PartyPokemon *mon);

// ============================================================================================================
// HP / status stamp
// ============================================================================================================

#define BOXHP_LOSS_BITS 10
#define BOXHP_LOSS_MASK 0x3FF

// Status codes (3 bits). Anything above BOXHP_CODE_FAINTED is not one of our stamps.
#define BOXHP_CODE_NONE      0
#define BOXHP_CODE_SLEEP     1
#define BOXHP_CODE_POISON    2 // also used for toxic: the toxic counter doesn't survive outside battle
#define BOXHP_CODE_BURN      3
#define BOXHP_CODE_FREEZE    4
#define BOXHP_CODE_PARALYSIS 5
#define BOXHP_CODE_FAINTED   6 // HP is 0 (a fainted Pokemon has no status)

// Status bits as listed in the comment above PartyOnlyPokemon (pokemon.h)
#define BOXHP_ST_SLEEP_MASK 0x07
#define BOXHP_ST_POISON     (1 << 3)
#define BOXHP_ST_BURN       (1 << 4)
#define BOXHP_ST_FREEZE     (1 << 5)
#define BOXHP_ST_PARALYSIS  (1 << 6)
#define BOXHP_ST_TOXIC      (1 << 7)

#define BOXHP_SLEEP_MIN_TURNS 2
#define BOXHP_SLEEP_MAX_TURNS 4

static u32 BoxHp_EncodeStatus(u32 st)
{
    if (st & (BOXHP_ST_POISON | BOXHP_ST_TOXIC)) return BOXHP_CODE_POISON;
    if (st & BOXHP_ST_BURN) return BOXHP_CODE_BURN;
    if (st & BOXHP_ST_FREEZE) return BOXHP_CODE_FREEZE;
    if (st & BOXHP_ST_PARALYSIS) return BOXHP_CODE_PARALYSIS;
    if (st & BOXHP_ST_SLEEP_MASK) return BOXHP_CODE_SLEEP;
    return BOXHP_CODE_NONE;
}

static u32 BoxHp_DecodeStatus(u32 code)
{
    switch (code) {
    case BOXHP_CODE_SLEEP:
        return BOXHP_SLEEP_MIN_TURNS + gf_rand() % (BOXHP_SLEEP_MAX_TURNS - BOXHP_SLEEP_MIN_TURNS + 1);
    case BOXHP_CODE_POISON: return BOXHP_ST_POISON;
    case BOXHP_CODE_BURN: return BOXHP_ST_BURN;
    case BOXHP_CODE_FREEZE: return BOXHP_ST_FREEZE;
    case BOXHP_CODE_PARALYSIS: return BOXHP_ST_PARALYSIS;
    default: return 0;
    }
}

// Writes the HP and status of a party Pokemon in its box header, right before it is stored
static void BoxHp_Stamp(struct PartyPokemon *mon)
{
    u32 hp, maxHp, loss;

    if (GetMonData(mon, MON_DATA_IS_EGG, NULL)) {
        mon->box.Unused = 0; // eggs have no meaningful HP
        return;
    }

    hp = GetMonData(mon, MON_DATA_HP, NULL);
    if (hp == 0) {
        mon->box.Unused = BOXHP_CODE_FAINTED << BOXHP_LOSS_BITS;
        return;
    }

    maxHp = GetMonData(mon, MON_DATA_MAXHP, NULL);
    loss = (maxHp > hp) ? (maxHp - hp) : 0;
    if (loss > BOXHP_LOSS_MASK) {
        loss = BOXHP_LOSS_MASK;
    }
    mon->box.Unused = loss | (BoxHp_EncodeStatus(GetMonData(mon, MON_DATA_STATUS, NULL)) << BOXHP_LOSS_BITS);
}

// Gives the stored HP and status back to a party Pokemon built from a box one.
// Must run after the stats were recalculated, which resets HP and status.
static void BoxHp_Apply(struct PartyPokemon *mon)
{
    u32 packed = mon->box.Unused;
    u32 code = packed >> BOXHP_LOSS_BITS;
    u32 st;
    u16 hp;

    if (packed == 0 || code > BOXHP_CODE_FAINTED) {
        return; // nothing stored (or garbage from outside the game): leave the Pokemon fully healed
    }

    if (code == BOXHP_CODE_FAINTED) {
        hp = 0;
    } else {
        u32 loss = packed & BOXHP_LOSS_MASK;
        u32 maxHp = GetMonData(mon, MON_DATA_MAXHP, NULL);

        hp = (loss >= maxHp) ? 0 : (maxHp - loss);
    }
    st = (hp == 0) ? 0 : BoxHp_DecodeStatus(code);
    SetMonData(mon, MON_DATA_HP, &hp);
    SetMonData(mon, MON_DATA_STATUS, &st);
}

// TRUE if this boxed Pokemon comes out fainted: read straight from its stamp, no temporary Pokemon
static BOOL BoxHp_IsFainted(const struct BoxPokemon *box)
{
    return (box->Unused >> BOXHP_LOSS_BITS) == BOXHP_CODE_FAINTED;
}

// Replaces CopyBoxPokemonToPokemon (hooked at its entry): every box -> party conversion gives back the
// stored HP and status. The source box Pokemon is never modified.
void LONG_CALL PM_CopyBoxPokemonToPokemon(const struct BoxPokemon *src, struct PartyPokemon *dest)
{
    u32 zero = 0;
    CAPSULE seals;
    struct Mail *mail;

    dest->box = *src;
    if (dest->box.box_lock) {
        dest->box.party_lock = TRUE;
    }

    SetMonData(dest, MON_DATA_STATUS, &zero);
    SetMonData(dest, MON_DATA_HP, &zero);
    SetMonData(dest, MON_DATA_MAXHP, &zero);

    mail = Mail_New(0); // heap id 0, as the original
    SetMonData(dest, MON_DATA_MAIL_STRUCT, mail);
    sys_FreeMemoryEz(mail);

    SetMonData(dest, MON_DATA_CAPSULE, &zero);
    MI_CpuClearFast(&seals, sizeof(seals));
    SetMonData(dest, MON_DATA_SEAL_COORDS, &seals);

    RecalcPartyPokemonStats(dest); // sets HP to full, as the original does

    BoxHp_Apply(dest);
    dest->box.Unused = 0; // a party Pokemon never carries a stamp: it would come back at the next deposit
}

// ============================================================================================================
// PC app hooks
// ============================================================================================================

// TRUE if the Pokemon in this box slot is an egg or comes out fainted
static BOOL PCUi_BoxSlotIsEggOrFainted(PCUiCtx *ctx, u32 slot)
{
    struct BoxPokemon *box;

    if (ov14_021E6070(ctx, slot, MON_DATA_IS_EGG, NULL)) {
        return TRUE;
    }
    box = ov14_021E60C0(ctx, ctx->boxno, slot);
    return box != NULL
        && GetBoxMonData(box, MON_DATA_SPECIES_EXISTS, NULL)
        && BoxHp_IsFainted(box);
}

// A party Pokemon holding mail or wearing a seal capsule can't be stored
static BOOL PCUi_PartyMonCanBeStored(struct PartyPokemon *mon)
{
    if (Func_02078068((u16)GetMonData(mon, MON_DATA_HELD_ITEM, NULL)) == 1) {
        return FALSE;
    }
    return GetMonData(mon, MON_DATA_CAPSULE, NULL) == 0;
}

// Replaces the "can this move be done" guard (021E690C). Returns TRUE if the move is allowed.
// Same logic as the original, plus: a fainted boxed Pokemon counts as an egg when it would replace
// your last able Pokemon.
BOOL LONG_CALL PM_PCUi_CanMoveSlot(PCUiCtx *ctx, u32 from, u32 to)
{
    u32 boxVar = ctx->boxno;
    u32 occupied = 0;

    if (to & PC_UI_TAB_FLAG) { // dropped on a box tab
        boxVar = ov14_021E6464(ctx, to);
        if (boxVar == ctx->boxno || PCStorage_CountMonsAndEggsInBox(ctx->storage, boxVar) == MONS_PER_BOX) {
            return FALSE;
        }
        to ^= PC_UI_TAB_FLAG;
    } else {
        occupied = ov14_021E6070(ctx, to, MON_DATA_SPECIES_EXISTS, NULL);
    }

    if (from >= PC_UI_PARTY_SLOT_BASE) { // a party Pokemon is moved
        if (!ov14_021E6480(ctx, from - PC_UI_PARTY_SLOT_BASE)) {
            // it is the last able Pokemon: it can only leave if a healthy one takes its place
            if (!occupied) {
                if (to < PC_UI_PARTY_SLOT_BASE || boxVar != ctx->boxno) {
                    return FALSE;
                }
            } else if (to < PC_UI_PARTY_SLOT_BASE && PCUi_BoxSlotIsEggOrFainted(ctx, to)) {
                return FALSE;
            }
        }
        if (to >= PC_UI_PARTY_SLOT_BASE) {
            return TRUE; // moved inside the party
        }
        return PCUi_PartyMonCanBeStored(Party_GetMonByIndex(ctx->party, from - PC_UI_PARTY_SLOT_BASE));
    }

    // a box Pokemon is moved
    if (to < PC_UI_PARTY_SLOT_BASE || !occupied) {
        return TRUE;
    }
    // it takes the place of the party Pokemon at "to", which goes to the box
    if (!PCUi_PartyMonCanBeStored(Party_GetMonByIndex(ctx->party, to - PC_UI_PARTY_SLOT_BASE))) {
        return FALSE;
    }
    if (!PCUi_BoxSlotIsEggOrFainted(ctx, from)) {
        return TRUE;
    }
    return ov14_021E6480(ctx, to - PC_UI_PARTY_SLOT_BASE) != 0; // an egg or a fainted one needs another able Pokemon
}

static void PCUi_RefreshBox(PCUiCtx *ctx)
{
    ov14_021F4958(ctx, ctx->boxno);
    ov14_021F4A20(ctx, ctx->boxno);
}

// Deposit: party -> box
void LONG_CALL PM_ov14_021E61BC(PCUiCtx *ctx, PCUiMove *mv)
{
    u8 zero = 0;
    struct PartyPokemon *mon = Party_GetMonByIndex(ctx->party, mv->srcSlot - PC_UI_PARTY_SLOT_BASE);

    SetMonData(mon, MON_DATA_MOOD, &zero);
    BoxHp_Stamp(mon);
    PCStorage_PlaceMonInBoxByIndexPair(ctx->storage, ctx->boxno, mv->dstSlot, &mon->box);
    ov14_021E6100(ctx, ctx->boxno, mv->srcSlot);
    PCUi_RefreshBox(ctx);
}

// Withdraw (ov14_021E6184) is not replaced: the original calls CopyBoxPokemonToPokemon, hooked above.

// Swap: party <-> box. The first argument is the box side, the second one the party side.
void LONG_CALL PM_ov14_021E611C(PCUiCtx *ctx, PCUiMove *boxMove, PCUiMove *partyMove)
{
    u8 zero = 0;
    u32 partyIdx = partyMove->srcSlot - PC_UI_PARTY_SLOT_BASE;
    struct BoxPokemon *src = ov14_021E60C0(ctx, ctx->boxno, boxMove->srcSlot);

    CopyBoxPokemonToPokemon(src, boxMove->mon); // hooked: applies the stored HP and status
    CopyPokemonToPokemon(Party_GetMonByIndex(ctx->party, partyIdx), partyMove->mon);
    SetMonData(partyMove->mon, MON_DATA_MOOD, &zero);
    BoxHp_Stamp(partyMove->mon);
    Party_SafeCopyMonToSlot_ResetAprijuiceModifiers(ctx->party, partyIdx, boxMove->mon);
    PCStorage_PlaceMonInBoxByIndexPair(ctx->storage, ctx->boxno, partyMove->dstSlot, &partyMove->mon->box);
    PCUi_RefreshBox(ctx);
}

#define GSYSTEM_BASE 0x021D110C // key struct: +0x34 button mode, +0x44/+0x48/+0x4C key words

static void ApplyButtonModeToKeys(u32 mode, u32 offset)
{
    volatile u32 *keys = (volatile u32 *)(GSYSTEM_BASE + offset);
    u32 v = *keys;

    if (mode == 1) {
        if (v & 0x0008) { // START also counts as X
            *keys = v | 0x0400;
        }
    } else {
        u32 swapped = 0; // X and Y swapped
        if (v & 0x0400) swapped |= 0x0800;
        if (v & 0x0800) swapped |= 0x0400;
        *keys = (v & 0xF3FF) | swapped;
    }
}

// Replaces the key remap (arm9 0201A5E8). Modes: 0 normal, 1 START=X, 2 X/Y swapped.
// Mode 3 (L=A, which also removed L and R) is gone: L stays a plain button.
void LONG_CALL PM_Sys_ApplyButtonMode(void)
{
    u32 mode = *(volatile u32 *)(GSYSTEM_BASE + 0x34);

    if (mode != 1 && mode != 2) {
        return;
    }
    ApplyButtonModeToKeys(mode, 0x48);
    ApplyButtonModeToKeys(mode, 0x44);
    ApplyButtonModeToKeys(mode, 0x4C);
}

// Portable PC: pressing L in the overworld opens the Pokemon Storage System
//
// Hooks:  PM_Field_CanTakeInput 0203E13C (1), PM_FieldSystem_CreateTask 020504F0 (3)
// rom.ld: Lpc_NewTask, Lpc_LaunchPC, Lpc_AfterPC, Lpc_RestoreOverworld
//         (the rest reuses existing symbols: FieldSystem_ApplicationIsRunning,
//          sys_AllocMemory, sys_FreeMemoryEz, GF_ASSERT_INTERNAL,
//          BeginNormalPaletteFade, WIPE_SYS_EndCheck)

#define FLAG_GOT_POKEGEAR 0x9C

// Key struct: +0x44 held keys, +0x48 keys pressed this frame
#define LPC_SYS_BASE        0x021D110C
#define LPC_KEYS_HELD       (*(volatile u32 *)(LPC_SYS_BASE + 0x44))
#define LPC_KEYS_PRESSED    (*(volatile u32 *)(LPC_SYS_BASE + 0x48))
#define LPC_PAD_L           0x0200
#define LPC_PAD_X           0x0400

#define LPC_START_MENU_TASK 0x0203BEF1 // Start menu task function (thumb)
#define LPC_HEAP_ID         11         // HEAP_ID_FIELD2, same heap as the script command
#define LPC_FADE_HEAP_ID    4          // HEAP_ID_FIELD1

// Start mode of the PC app (operand of ScrCmd_158): 0 = Deposit, 2 = Move Pokemon
#define LPC_PC_MODE_MOVE_POKEMON 2

// BeginNormalPaletteFade arguments, as used by the FadeScreen script command
#define LPC_FADE_BOTH_SCREENS 0
#define LPC_FADE_OUT          0 // FADE_TYPE_BRIGHTNESS_OUT
#define LPC_FADE_IN           1 // FADE_TYPE_BRIGHTNESS_IN
#define LPC_FADE_COLOR_BLACK  0
#define LPC_FADE_STEPS        6
#define LPC_FADE_FRAMES       1

// FieldSystem offsets
#define LPC_FIELD_SUB       0x00 // pointer to a struct: +8 input lock
#define LPC_FIELD_SAVEDATA  0x0C
#define LPC_FIELD_TASK      0x10 // current task manager (0 = none)
#define LPC_FIELD_UNK6C     0x6C
#define LPC_FIELD_PC_ARGS   0x10C // menu input state, passed to the PC app

// TaskManager offsets
#define LPC_TASK_WORK       0x0C
#define LPC_TASK_FIELD      0x18

typedef struct PCLaunchArgs { // same layout as PCBoxArgs
    void *saveData;
    void *menuInputState;
    int mode;
} PCLaunchArgs;

typedef struct PCLaunchWork {
    PCLaunchArgs args;
    u32 state;
} PCLaunchWork;

enum {
    PC_STATE_FADE_OUT,
    PC_STATE_FADE_OUT_WAIT,
    PC_STATE_APP_WAIT,
    PC_STATE_FADE_IN,
    PC_STATE_FADE_IN_WAIT,
};

void *LONG_CALL Lpc_NewTask(void *field, void *func, void *work);
void LONG_CALL Lpc_LaunchPC(void *field, void *args);
void LONG_CALL Lpc_AfterPC(void *field);
void LONG_CALL Lpc_RestoreOverworld(void *taskMan);

BOOL LONG_CALL FieldSystem_ApplicationIsRunning(void *field);
void LONG_CALL BeginNormalPaletteFade(u32 fadeMode, u32 typeMain, u32 typeSub, u16 color, int steps, int framesPerStep, u32 heapId);

// The portable PC is only available once the player has the PokeGear.
static BOOL PCLaunch_IsUnlocked(void)
{
    return CheckScriptFlag(FLAG_GOT_POKEGEAR);
}

static void PCLaunch_StartFade(u32 type)
{
    BeginNormalPaletteFade(LPC_FADE_BOTH_SCREENS, type, type, LPC_FADE_COLOR_BLACK,
                           LPC_FADE_STEPS, LPC_FADE_FRAMES, LPC_FADE_HEAP_ID);
}

// Task function: returns 1 when finished
static u32 PCLaunchTask(void *taskMan)
{
    void *field = *(void **)((u8 *)taskMan + LPC_TASK_FIELD);
    PCLaunchWork *work = *(PCLaunchWork **)((u8 *)taskMan + LPC_TASK_WORK);

    switch (work->state) {
    case PC_STATE_FADE_OUT:
        PCLaunch_StartFade(LPC_FADE_OUT);
        work->state = PC_STATE_FADE_OUT_WAIT;
        return 0;
    case PC_STATE_FADE_OUT_WAIT:
        if (!WIPE_SYS_EndCheck()) {
            return 0;
        }
        Lpc_LaunchPC(field, &work->args);
        work->state = PC_STATE_APP_WAIT;
        return 0;
    case PC_STATE_APP_WAIT:
        if (FieldSystem_ApplicationIsRunning(field)) {
            return 0;
        }
        Lpc_AfterPC(field);
        Lpc_RestoreOverworld(taskMan); // sub-task: this task resumes when it finishes
        work->state = PC_STATE_FADE_IN;
        return 0;
    case PC_STATE_FADE_IN:
        PCLaunch_StartFade(LPC_FADE_IN);
        work->state = PC_STATE_FADE_IN_WAIT;
        return 0;
    case PC_STATE_FADE_IN_WAIT:
        if (!WIPE_SYS_EndCheck()) {
            return 0;
        }
        sys_FreeMemoryEz(work);
        return 1;
    }
    return 0;
}

// Replaces 0203E13C. Same conditions as the original; L is turned into X
// so the game starts creating the Start menu task, which CreateTask swaps below.
BOOL LONG_CALL PM_Field_CanTakeInput(void *field)
{
    u8 *f = (u8 *)field;
    u32 *sub = *(u32 **)(f + LPC_FIELD_SUB);

    if (sub[2] != 0 || *(u32 *)(f + LPC_FIELD_UNK6C) == 0 || *(u32 *)(f + LPC_FIELD_TASK) != 0) {
        return FALSE;
    }
    if ((LPC_KEYS_PRESSED & LPC_PAD_L) && PCLaunch_IsUnlocked()) {
        LPC_KEYS_PRESSED |= LPC_PAD_X;
        LPC_KEYS_HELD |= LPC_PAD_X;
    }
    return TRUE;
}

// Replaces 020504F0. Same behaviour as the original, except that the Start menu
// task is replaced by the PC launch task when L is pressed.
void LONG_CALL PM_FieldSystem_CreateTask(void *field, void *func, void *work)
{
    u8 *f = (u8 *)field;
    u32 *taskSlot = (u32 *)(f + LPC_FIELD_TASK);

    if (*taskSlot != 0) {
        GF_ASSERT_INTERNAL();
    }

    if ((LPC_KEYS_PRESSED & LPC_PAD_L) && PCLaunch_IsUnlocked() && (u32)func == LPC_START_MENU_TASK) {
        PCLaunchWork *w = sys_AllocMemory(LPC_HEAP_ID, sizeof(PCLaunchWork));

        sys_FreeMemoryEz(work); // the Start menu's work block, which won't be used
        w->args.saveData = *(void **)(f + LPC_FIELD_SAVEDATA);
        w->args.menuInputState = f + LPC_FIELD_PC_ARGS;
        w->args.mode = LPC_PC_MODE_MOVE_POKEMON;
        w->state = PC_STATE_FADE_OUT;
        func = (void *)PCLaunchTask;
        work = w;
    }
    *taskSlot = (u32)Lpc_NewTask(field, func, work);
}

#endif
#endif