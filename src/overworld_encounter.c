#include "types.h"
#include "pokemon.h"
#include "battle.h"
#include "map_events_internal.h"

int LONG_CALL PlayerAvatar_GetXCoord(FIELD_PLAYER_AVATAR *avatar);
int LONG_CALL PlayerAvatar_GetZCoord(FIELD_PLAYER_AVATAR *avatar);
int LONG_CALL PlayerAvatar_GetFacingDirection(FIELD_PLAYER_AVATAR *avatar);

#define OW_DATA __attribute__((section(".data")))

#define OWENC_NORMAL_MAX  10
#define OWENC_SHINY_MAX   2
#define OWENC_SLOTS       (OWENC_NORMAL_MAX + OWENC_SHINY_MAX)
#define OWENC_SCREEN_MAX  10     /* Pokemon non shiny nella finestra visibile */
#define OWENC_WIN_X       8
#define OWENC_WIN_Z       6
#define OWENC_RADIUS      6
#define OWENC_TRIES       80
#define OWENC_RANGE       4
#define OWENC_MIN_DIST    3      /* distanza minima tra Pokemon (Chebyshev) */
#define OWENC_GRACE       6      /* passi senza contatto dopo una battaglia non conclusa */
#define OWENC_COOLDOWN       300   /* frame tra uno spawn e il successivo */
#define OWENC_SPAWN_INTERVAL 60    /* frame tra un tentativo di spawn e il successivo */
#define OWENC_MOVE_MIN       40    /* frame minimi tra due passi di un Pokemon */
#define OWENC_MOVE_RAND      120   /* + casuale 0..119 (totale < 256, idle e' u8) */
#define OWENC_OBJ_ID      0xC0
#define MAP_NOTHING_ID    1
#define HEAP_FIELD2       11

#define OWENC_R_WIN        1
#define OWENC_R_CAUGHT     4
#define OWENC_R_FOE_FLED   6

#define PAD_KEY_RIGHT_ 0x0010
#define PAD_KEY_LEFT_  0x0020
#define PAD_KEY_UP_    0x0040
#define PAD_KEY_DOWN_  0x0080

#define Ow_Save_VarsFlags_Get    ((void *(*)(void *))0x020503D1)
#define Ow_CheckPalPark          ((BOOL (*)(void *))0x020668A1)
#define Ow_CheckSafari           ((BOOL (*)(void *))0x02066861)
#define Ow_CheckBugContest       ((BOOL (*)(void *))0x02066871)
#define Ow_GetWildEffect         ((int (*)(struct BattleSetup *))0x020517E9)
#define Ow_GetWildBgm            ((int (*)(struct BattleSetup *))0x020517FD)
#define Ow_Encounter_New         ((void *(*)(struct BattleSetup *, int, int, u32 *))0x020506BD)
#define Ow_WildEncounter_New     ((void *(*)(struct BattleSetup *, int, int, u32 *))0x02050ACD)
#define Ow_CreateTask            ((void *(*)(FieldSystem *, void *, void *))0x020504F1)
#define Ow_CatchingShow_Check    ((BOOL (*)(FieldSystem *, int, int))0x02055875)
#define Ow_CatchingShow_GetData  ((void *(*)(FieldSystem *))0x0205589D)
#define Ow_PalParkBattle         ((void (*)(FieldSystem *, void *))0x020511F9)
#define Ow_HasWildEncounters     ((BOOL (*)(int))0x0203B325)
#define Ow_GetStandingTileCoords ((void (*)(FieldSystem *, int *, int *))0x021E7AB9)
#define Ow_Perform               ((BOOL (*)(FieldSystem *))0x02246C8D)
#define Ow_GetTileEncType        ((u8 (*)(FieldSystem *, u8, u8 *))0x0224762D)
#define Ow_MapObject_Remove      ((void (*)(LocalMapObject *))0x0205E3CD)
#define Ow_GetMonGender          ((u8 (*)(struct PartyPokemon *))0x0206FF89)
#define Ow_BattleSetup_Delete    ((void (*)(struct BattleSetup *))0x02051BF9)
#define Ow_Sub_0205239C ((void (*)(struct BattleSetup *, FieldSystem *))0x0205239D)

#define OW_TASK_SAFARI ((void *)0x02050D1D)
#define OW_TASK_BUG    ((void *)0x02050EB9)
#define OW_TASK_WILD   ((void *)0x02050C19)

#define SLOT_EMPTY   0
#define SLOT_ACTIVE  1
#define SLOT_BATTLE  2

typedef struct {
    struct PartyPokemon mon;     /* copia del Pokemon generato */
    LocalMapObject *obj;
    u32  sprite;
    u16  species;
    u16  mapId;
    u16  homeX, homeZ, curX, curZ;
    u16  age;
    u8   form, gender;
    u8   state;
    u8   keep;                   /* shiny persistente */
    u8   idle;
    u8   dir;
    u8   grace;
} OwEnc;

static OwEnc               sOwEnc[OWENC_SLOTS]  OW_DATA;
static int                 sMap                 OW_DATA = -1;
static int                 sInCheck             OW_DATA = 0;
static int                 sStarted             OW_DATA = 0;
static int                 sCooldown            OW_DATA = 0;
static MapObjectMan       *sObjMan              OW_DATA = NULL;
static int                 sFieldGone           OW_DATA = 0;
static struct BattleSetup *sPending             OW_DATA = NULL;
static int                 sPendingIdx          OW_DATA = -1;
static LocalMapObject *sGhostObj     OW_DATA = NULL;
static u32             sGhostSprite  OW_DATA = 0;
static int             sGhostSpecies OW_DATA = 0;
static int sSpawnTimer OW_DATA = 0;
static int sTimerCall  OW_DATA = 0;

/* ===== util ===== */
static int OwEnc_TileFromAny(int v){
    if (v < 0) v = -v;
    return (v >= 65536) ? (v >> 16) : v;
}
static int OwEnc_PlayerX(FieldSystem *fs){ return OwEnc_TileFromAny(PlayerAvatar_GetXCoord(fs->playerAvatar)); }
static int OwEnc_PlayerZ(FieldSystem *fs){ return OwEnc_TileFromAny(PlayerAvatar_GetZCoord(fs->playerAvatar)); }
static int OwEnc_Abs(int v){ return v < 0 ? -v : v; }

static void OwEnc_CopyMon(struct PartyPokemon *dst, const struct PartyPokemon *src){
    u32 *d = (u32 *)dst; const u32 *s = (const u32 *)src;
    for (u32 i = 0; i < sizeof(struct PartyPokemon) / 4; i++) d[i] = s[i];
}

static BOOL OwEnc_InWindow(FieldSystem *fs, int x, int z){
    return OwEnc_Abs(x - OwEnc_PlayerX(fs)) <= OWENC_WIN_X
        && OwEnc_Abs(z - OwEnc_PlayerZ(fs)) <= OWENC_WIN_Z;
}

/* ===== lifecycle ===== */
static void OwEnc_Forget(OwEnc *e){
    e->obj = NULL; e->sprite = 0;
    e->species = 0; e->form = 0; e->gender = 0; e->mapId = 0xFFFF;
    e->homeX = e->homeZ = e->curX = e->curZ = 0;
    e->age = 0; e->state = SLOT_EMPTY; e->keep = 0; e->idle = 0; e->dir = 1; e->grace = 0;
}
static BOOL OwEnc_ObjValid(OwEnc *e){
    return e->obj && (u32)MapObject_GetGfxID(e->obj) == e->sprite;
}
static void OwEnc_RemoveObj(OwEnc *e){
    if (OwEnc_ObjValid(e)) Ow_MapObject_Remove(e->obj);
    e->obj = NULL;
}
static void OwEnc_FreeSlot(OwEnc *e){
    OwEnc_RemoveObj(e);
    OwEnc_Forget(e);
}

/* Rimuove dalla mappa ogni oggetto con l'ID dello slot i (anche se il nostro puntatore e' perso). */
static void OwEnc_PurgeSlotObjects(FieldSystem *fs, int i){
    LocalMapObject *prev = NULL;
    for (int n = 0; n < 4; n++) {
        LocalMapObject *o = GetMapObjectByID(fs->mapObjectMan, OWENC_OBJ_ID + i);
        if (!o || o == prev) break;     /* nessuno, oppure non e' stato rimosso: non insistere */
        Ow_MapObject_Remove(o);
        prev = o;
    }
}
static void OwEnc_PurgeAll(FieldSystem *fs){
    for (int i = 0; i < OWENC_SLOTS; i++) OwEnc_PurgeSlotObjects(fs, i);
}

/* ===== oggetto ===== */
static BOOL OwEnc_BuildObject(FieldSystem *fs, int i){
    OwEnc *e = &sOwEnc[i];
    OwEnc_PurgeSlotObjects(fs, i);
    LocalMapObject *o = CreateSpecialFieldObject(fs->mapObjectMan, e->curX, e->curZ, e->dir,
                                                 e->sprite,
                                                 0,
                                                 MAP_NOTHING_ID);
    if (!o) return FALSE;
    MapObject_SetID(o, OWENC_OBJ_ID + i);
    MapObject_SetType(o, 0);
    MapObject_SetFlagID(o, 0);
    MapObject_SetXRange(o, OWENC_RANGE);
    MapObject_SetYRange(o, OWENC_RANGE);
    FollowPokeMapObjectSetParams(o, e->species, e->form, MonIsShiny(&e->mon));
    MapObject_SetBits(o, MAPOBJECTFLAG_ACTIVE | MAPOBJECTFLAG_UNK13 | MAPOBJECTFLAG_UNK10);
    MapObject_ClearBits(o, MAPOBJECTFLAG_UNK8 | MAPOBJECTFLAG_UNK7);
    MapObject_SetFlag29(o, FALSE);
    e->obj = o;
    return TRUE;
}

static BOOL OwEnc_IsWalkable(FieldSystem *fs, int x, int z){
    if (x < 0 || z < 0) return FALSE;
    u8 type, beh = GetMetatileBehaviorAt(fs, x, z);
    return Ow_GetTileEncType(fs, beh, &type) != 0;
}
/* TRUE se (x,z) e' a meno di OWENC_MIN_DIST tile da un altro Pokemon */
static BOOL OwEnc_Occupied(int x, int z){
    for (int i = 0; i < OWENC_SLOTS; i++) {
        if (sOwEnc[i].state == SLOT_EMPTY || (int)sOwEnc[i].mapId != sMap) continue;
        int dx = OwEnc_Abs(sOwEnc[i].curX - x), dz = OwEnc_Abs(sOwEnc[i].curZ - z);
        if ((dx > dz ? dx : dz) < OWENC_MIN_DIST) return TRUE;
    }
    return FALSE;
}

static void OwEnc_ClearGhost(void){
    if (sGhostObj
        && (u32)MapObject_GetGfxID(sGhostObj) == sGhostSprite
        && MapObject_GetParam(sGhostObj, 0) == sGhostSpecies)
        Ow_MapObject_Remove(sGhostObj);
    sGhostObj = NULL;
}

/* ===== battaglia ===== */
static void OwEnc_StartBattle(FieldSystem *fs, struct BattleSetup *setup){
    sStarted = 1;
    void *flags = Ow_Save_VarsFlags_Get(fs->savedata);
    int effect = Ow_GetWildEffect(setup);
    int bgm = Ow_GetWildBgm(setup);
    void *enc, *task;
    if (Ow_CheckSafari(flags)) {
        enc = Ow_Encounter_New(setup, effect, bgm, NULL); task = OW_TASK_SAFARI;
    } else if (Ow_CheckBugContest(flags)) {
        enc = Ow_Encounter_New(setup, effect, bgm, NULL); task = OW_TASK_BUG;
    } else {
        enc = Ow_WildEncounter_New(setup, effect, bgm, NULL); task = OW_TASK_WILD;
    }
    Ow_CreateTask(fs, task, enc);
}

/* Sostituisce sub_02050724: chiamata dal task subito dopo la battaglia. */
void OwEnc_BattleDone(struct BattleSetup *setup, FieldSystem *fs){
    if (setup == sPending && sPendingIdx >= 0) {
        OwEnc_ClearGhost();                    /* nuova riga */
        OwEnc_Forget(&sOwEnc[sPendingIdx]);
        sPending = NULL; sPendingIdx = -1;
        sFieldGone = 1;
    }
    if (!(setup->battleType & 0x80000000u)) Ow_Sub_0205239C(setup, fs);
}

static BOOL OwEnc_StartContact(FieldSystem *fs, int idx){
    OwEnc *e = &sOwEnc[idx];
    struct BattleSetup *s = BattleSetup_New(HEAP_FIELD2, 0);
    if (!s) return FALSE;
    BattleSetup_InitFromFieldSystem(s, fs);
    if (!PokeParty_Add(s->party[1], &e->mon)) { Ow_BattleSetup_Delete(s); return FALSE; }
    sGhostObj = e->obj;              /* l'oggetto resta visibile fino a fine battaglia */
    sGhostSprite = e->sprite;
    sGhostSpecies = e->species;
    e->state = SLOT_BATTLE;
    sPending = s; sPendingIdx = idx;
    OwEnc_StartBattle(fs, s);
    return TRUE;
}

/* ===== sync ===== */
static void OwEnc_Sync(FieldSystem *fs){
    int map = fs->location->mapId;
    MapObjectMan *man = fs->mapObjectMan;

    if (sObjMan != man || sFieldGone) {
        OwEnc_ClearGhost();
        OwEnc_PurgeAll(fs);
        for (int i = 0; i < OWENC_SLOTS; i++) {
            OwEnc *e = &sOwEnc[i];
            e->obj = NULL;
            if (e->state == SLOT_BATTLE) OwEnc_Forget(e);
        }
        sObjMan = man; sFieldGone = 0;
        sPending = NULL; sPendingIdx = -1;
    }
    if (sMap != map) {
        for (int i = 0; i < OWENC_SLOTS; i++) {
            OwEnc *e = &sOwEnc[i];
            if (e->state == SLOT_EMPTY) continue;
            if (e->keep) OwEnc_RemoveObj(e);
            else OwEnc_FreeSlot(e);
        }
        OwEnc_PurgeAll(fs);
        sMap = map;
    }
    for (int i = 0; i < OWENC_SLOTS; i++) {
        OwEnc *e = &sOwEnc[i];
        if (e->state != SLOT_ACTIVE) continue;
        if (OwEnc_ObjValid(e)) {
            if (e->age < 0xFFFF) e->age++;
            continue;
        }
        e->obj = NULL;
        if (e->mapId == map) OwEnc_BuildObject(fs, i);
    }
}

/* ===== wander manuale: rimuove e ricrea l'oggetto sulla nuova tile ===== */
static void OwEnc_Wander(FieldSystem *fs){
    int px = OwEnc_PlayerX(fs), pz = OwEnc_PlayerZ(fs);
    for (int i = 0; i < OWENC_SLOTS; i++) {
        OwEnc *e = &sOwEnc[i];
        if (e->state != SLOT_ACTIVE || !OwEnc_ObjValid(e)) continue;
        if (e->idle > 0) { e->idle--; continue; }
        e->idle = (u8)(OWENC_MOVE_MIN + gf_rand() % OWENC_MOVE_RAND);

        int nx = e->curX, nz = e->curZ;
        u8 nd;
        switch (gf_rand() & 3) {
            case 0:  nz--; nd = 0; break;
            case 1:  nz++; nd = 1; break;
            case 2:  nx--; nd = 2; break;
            default: nx++; nd = 3; break;
        }
        if (nx < 0 || nz < 0
            || OwEnc_Abs(nx - e->homeX) > OWENC_RANGE || OwEnc_Abs(nz - e->homeZ) > OWENC_RANGE
            || (nx == px && nz == pz)
            || !OwEnc_IsWalkable(fs, nx, nz)) continue;
        u16 ox = e->curX, oz = e->curZ;             /* ignora se stesso nel controllo distanza */
        e->curX = 0xFFFF; e->curZ = 0xFFFF;
        BOOL busy = OwEnc_Occupied(nx, nz);
        e->curX = ox; e->curZ = oz;
        if (busy) continue;
        e->curX = (u16)nx; e->curZ = (u16)nz; e->dir = nd;
        OwEnc_RemoveObj(e);
        OwEnc_BuildObject(fs, i);
    }
}

/* Chiamata a ogni frame dal hook. Contatto: giocatore fermo che spinge verso il Pokemon. */
void OwEnc_Frame(FieldSystem *fs, u16 newKeys, u16 heldKeys){
    (void)newKeys;
    if (Ow_CheckPalPark(Ow_Save_VarsFlags_Get(fs->savedata)) == 1) return;
    OwEnc_Sync(fs);

    int px = OwEnc_PlayerX(fs), pz = OwEnc_PlayerZ(fs);
    for (int i = 0; i < OWENC_SLOTS; i++) {
        OwEnc *e = &sOwEnc[i];
        if (e->state != SLOT_ACTIVE || e->mapId != fs->location->mapId || !OwEnc_ObjValid(e)) continue;
        int dx = e->curX - px, dz = e->curZ - pz;
        BOOL hit = (dx == 0 && dz == 0)
                || (dx == 0 && dz == -1 && (heldKeys & PAD_KEY_UP_))
                || (dx == 0 && dz ==  1 && (heldKeys & PAD_KEY_DOWN_))
                || (dx == -1 && dz == 0 && (heldKeys & PAD_KEY_LEFT_))
                || (dx ==  1 && dz == 0 && (heldKeys & PAD_KEY_RIGHT_));
        if (hit) { OwEnc_StartContact(fs, i); return; }
    }

    OwEnc_Wander(fs);

    if (sCooldown > 0) sCooldown--;
    if (++sSpawnTimer < OWENC_SPAWN_INTERVAL) return;
    sSpawnTimer = 0;
    if (sCooldown > 0 || !Ow_HasWildEncounters(fs->location->mapId)) return;

    u16 *inhibit = (u16 *)((u8 *)fs + 0x7E);
    u16 saved = *inhibit;
    *inhibit = 0xFFFF;                  /* Perform esce subito se i passi sono <= 3 */
    sStarted = 0; sInCheck = 1; sTimerCall = 1;
    BOOL r = Ow_Perform(fs);
    sInCheck = 0; sTimerCall = 0;
    if (!r) *inhibit = saved;
}

/* Sostituisce a 021E6B9C: bl sub_0205DD94 / adds r7,r0 / ldr r0,[r4,#12] */
__attribute__((naked)) void OwEnc_FrameHook(void){
    __asm__ volatile(
        ".thumb\n"
        "push {r0-r3}\n"
        "mov r0, r4\n"              /* r1, r2 = newKeys, heldKeys restano com'erano */
        "ldr r3, =OwEnc_Frame\n"
        "blx r3\n"
        "pop {r0-r3}\n"
        "ldr r3, =0x0205DD95\n"     /* sub_0205DD94 originale, Thumb */
        "blx r3\n"
        "mov r7, r0\n"
        "ldr r0, [r4, #12]\n"
        "ldr r3, =0x021E6BA5\n"     /* riprende da 021E6BA4, Thumb */
        "bx r3\n"
        ".ltorg\n"
    );
}

/* ===== spawn ===== */
static int OwEnc_CountScreen(FieldSystem *fs){
    int n = 0;
    for (int i = 0; i < OWENC_SLOTS; i++)
        if (sOwEnc[i].state != SLOT_EMPTY && OwEnc_InWindow(fs, sOwEnc[i].curX, sOwEnc[i].curZ)) n++;
    return n;
}
static int OwEnc_CountNormal(void){
    int n = 0;
    for (int i = 0; i < OWENC_SLOTS; i++)
        if (sOwEnc[i].state != SLOT_EMPTY && !sOwEnc[i].keep) n++;
    return n;
}
static int OwEnc_CountShiny(void){
    int n = 0;
    for (int i = 0; i < OWENC_SLOTS; i++)
        if (sOwEnc[i].state != SLOT_EMPTY && sOwEnc[i].keep) n++;
    return n;
}
static int OwEnc_FindFree(void){
    for (int i = 0; i < OWENC_SLOTS; i++) if (sOwEnc[i].state == SLOT_EMPTY) return i;
    return -1;
}
/* il normale piu' vecchio, preferibilmente fuori dalla finestra */
static int OwEnc_PickVictim(FieldSystem *fs){
    int best = -1, bestOut = 0; u16 bestAge = 0;
    for (int i = 0; i < OWENC_SLOTS; i++) {
        OwEnc *e = &sOwEnc[i];
        if (e->state != SLOT_ACTIVE || e->keep) continue;
        int out = !OwEnc_InWindow(fs, e->curX, e->curZ);
        if (best < 0 || (out && !bestOut) || (out == bestOut && e->age > bestAge)) {
            best = i; bestOut = out; bestAge = e->age;
        }
    }
    return best;
}
static BOOL OwEnc_PickSpawnTile(FieldSystem *fs, int *outX, int *outZ){
    int px = OwEnc_PlayerX(fs), pz = OwEnc_PlayerZ(fs);
    for (int t = 0; t < OWENC_TRIES; t++) {
        int dx = (int)(gf_rand() % (OWENC_RADIUS * 2 + 1)) - OWENC_RADIUS;
        int dz = (int)(gf_rand() % (OWENC_RADIUS * 2 + 1)) - OWENC_RADIUS;
        if (OwEnc_Abs(dx) + OwEnc_Abs(dz) < 3) continue;
        if (!OwEnc_IsWalkable(fs, px + dx, pz + dz)) continue;
        if (OwEnc_Occupied(px + dx, pz + dz)) continue;
        *outX = px + dx; *outZ = pz + dz;
        return TRUE;
    }
    return FALSE;
}

/* 0 = spawnato, 1 = scartato (setup da liberare), 2 = non supportato (battaglia classica) */
static int OwEnc_TrySpawn(FieldSystem *fs, struct BattleSetup *setup){
    struct PartyPokemon *mon = Party_GetMonByIndex(setup->party[1], 0);
    int species = GetMonData(mon, MON_DATA_SPECIES, 0);
    if (species <= 0 || species > 493) return 2;
    if (sCooldown > 0) return 1;

    BOOL shiny = MonIsShiny(mon);
    if (!shiny && OwEnc_CountScreen(fs) >= OWENC_SCREEN_MAX) return 1;

    int x, z;
    if (!OwEnc_PickSpawnTile(fs, &x, &z)) return 1;

    BOOL wantKeep = shiny && OwEnc_CountShiny() < OWENC_SHINY_MAX;
    int slot = OwEnc_FindFree();
    if (slot < 0 || (!wantKeep && OwEnc_CountNormal() >= OWENC_NORMAL_MAX)) {
        int v = OwEnc_PickVictim(fs);
        if (v < 0) return 1;
        OwEnc_FreeSlot(&sOwEnc[v]);
        slot = v;
    }

    OwEnc *e = &sOwEnc[slot];
    OwEnc_Forget(e);
    OwEnc_CopyMon(&e->mon, mon);
    e->species = (u16)species;
    e->form = (u8)GetMonData(mon, MON_DATA_FORM, 0);
    e->gender = Ow_GetMonGender(mon);
    e->sprite = FollowingPokemon_GetSpriteID(species, e->form, e->gender);
    e->mapId = (u16)fs->location->mapId;
    e->homeX = e->curX = (u16)x;
    e->homeZ = e->curZ = (u16)z;
    e->dir = (u8)(gf_rand() & 3);
    e->keep = wantKeep ? 1 : 0;
    e->state = SLOT_ACTIVE;
    if (!OwEnc_BuildObject(fs, slot)) { OwEnc_Forget(e); return 1; }
    sCooldown = OWENC_COOLDOWN;
    return 0;
}

/* Sostituisce sub_02050B08 */
void OwEnc_Capture(FieldSystem *fs, struct BattleSetup *setup){
    if (!sInCheck || setup->battleType != 0) {      /* roamer, safari, gara bug, altro */
        if (sTimerCall) { Ow_BattleSetup_Delete(setup); return; }
        OwEnc_StartBattle(fs, setup);
        return;
    }
    if (OwEnc_TrySpawn(fs, setup) == 2) {
        if (sTimerCall) { Ow_BattleSetup_Delete(setup); return; }
        OwEnc_StartBattle(fs, setup);
        return;
    }
    Ow_BattleSetup_Delete(setup);
}

/* Sostituisce FieldSystem_CheckWildEncounter */
BOOL OwEnc_CheckWildEncounter(FieldSystem *fs){
    u16 *inhibit = (u16 *)((u8 *)fs + 0x7E);
    if (*inhibit < 0xFFFF) (*inhibit)++;
    int x, z;
    Ow_GetStandingTileCoords(fs, &x, &z);
    if (Ow_CheckPalPark(Ow_Save_VarsFlags_Get(fs->savedata)) == 1) {
        if (Ow_CatchingShow_Check(fs, x, z) == 1) {
            Ow_PalParkBattle(fs, Ow_CatchingShow_GetData(fs));
            return TRUE;
        }
        return FALSE;
    }
    OwEnc_Sync(fs);
    if (!Ow_HasWildEncounters(fs->location->mapId)) return FALSE;
    sStarted = 0; sInCheck = 1;
    Ow_Perform(fs);
    sInCheck = 0;
    return sStarted;
}