#include "types.h"
#include "pokemon.h"
#include "battle.h"
#include "map_events_internal.h"

int LONG_CALL PlayerAvatar_GetXCoord(FIELD_PLAYER_AVATAR *avatar);
int LONG_CALL PlayerAvatar_GetZCoord(FIELD_PLAYER_AVATAR *avatar);

#define OW_DATA __attribute__((section(".data")))

/* 0 = wander manuale (solo a fine passo del giocatore); 1 = movimento nativo */
#define OWENC_NATIVE      1
/* usato solo se OWENC_NATIVE == 1: numero del movimento "cammina a caso" in DSPRE.
   ATTENZIONE: 2 e' il valore che ricordo, verificalo in DSPRE/pokeheartgold. */
#define OWENC_MOVE_TYPE   2

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
#define OWENC_COOLDOWN    10     /* passi tra uno spawn e il successivo */
#define OWENC_GRACE       6      /* passi senza contatto dopo una battaglia non conclusa */
#define OWENC_STEP_CHANCE 25
#define OWENC_OBJ_ID      0xC0
#define MAP_NOTHING_ID    1
#define HEAP_FIELD2       11

#define OWENC_R_WIN        1
#define OWENC_R_CAUGHT     4
#define OWENC_R_FOE_FLED   6

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
#define Ow_Sub_0205239C          ((void (*)(struct BattleSetup *))0x0205239D)

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

static OwEnc               sOwEnc[OWENC_SLOTS] OW_DATA;
static int                 sMap        OW_DATA = -1;
static int                 sInCheck    OW_DATA = 0;
static int                 sStarted    OW_DATA = 0;
static int                 sCooldown   OW_DATA = 0;
static MapObjectMan       *sObjMan     OW_DATA = NULL;
static int                 sFieldGone  OW_DATA = 0;
static struct BattleSetup *sPending    OW_DATA = NULL;
static int                 sPendingIdx OW_DATA = -1;

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

static BOOL OwEnc_IsWalkable(FieldSystem *fs, int x, int z){
    if (x < 0 || z < 0) return FALSE;
    u8 type, beh = GetMetatileBehaviorAt(fs, x, z);
    return Ow_GetTileEncType(fs, beh, &type) != 0;
}
/* TRUE se (x,z) e' a meno di OWENC_MIN_DIST tile da un altro Pokemon */
static BOOL OwEnc_Occupied(int x, int z){
    for (int i = 0; i < OWENC_SLOTS; i++) {
        if (sOwEnc[i].state == SLOT_EMPTY) continue;
        int dx = OwEnc_Abs(sOwEnc[i].curX - x), dz = OwEnc_Abs(sOwEnc[i].curZ - z);
        if ((dx > dz ? dx : dz) < OWENC_MIN_DIST) return TRUE;
    }
    return FALSE;
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
void OwEnc_BattleDone(struct BattleSetup *setup){
    if (setup == sPending && sPendingIdx >= 0) {
        OwEnc *e = &sOwEnc[sPendingIdx];
        int r = setup->winFlag;
        if (r == OWENC_R_WIN || r == OWENC_R_CAUGHT || r == OWENC_R_FOE_FLED) {
            OwEnc_Forget(e);
        } else if (e->state == SLOT_BATTLE) {      /* fuga tua o sconfitta: resta */
            e->state = SLOT_ACTIVE;
            e->grace = OWENC_GRACE;
        }
        sPending = NULL; sPendingIdx = -1;
        sFieldGone = 1;
    }
    /* sempre: riporta squadra (exp, PP, HP) nel salvataggio */
    Ow_Sub_0205239C(setup);
}

static BOOL OwEnc_StartContact(FieldSystem *fs, int idx){
    OwEnc *e = &sOwEnc[idx];
    struct BattleSetup *s = BattleSetup_New(HEAP_FIELD2, 0);
    if (!s) return FALSE;
    BattleSetup_InitFromFieldSystem(s, fs);
    if (!PokeParty_Add(s->party[1], &e->mon)) { Ow_BattleSetup_Delete(s); return FALSE; }
    OwEnc_RemoveObj(e);
    e->state = SLOT_BATTLE;
    sPending = s; sPendingIdx = idx;
    OwEnc_StartBattle(fs, s);
    return TRUE;
}

/* ===== oggetto ===== */
static BOOL OwEnc_BuildObject(FieldSystem *fs, int i){
    OwEnc *e = &sOwEnc[i];
    LocalMapObject *o = CreateSpecialFieldObject(fs->mapObjectMan, e->curX, e->curZ, e->dir,
                                                 e->sprite,
                                                 OWENC_NATIVE ? OWENC_MOVE_TYPE : 0,
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
    MapObject_SetFlag29(o, TRUE);
    e->obj = o;
    return TRUE;
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
        OwEnc_StartBattle(fs, setup);
        return;
    }
    if (OwEnc_TrySpawn(fs, setup) == 2) {
        OwEnc_StartBattle(fs, setup);
        return;
    }
    Ow_BattleSetup_Delete(setup);                  /* il mon e' gia' copiato nello slot */
}

/* ===== sync ===== */
static void OwEnc_Sync(FieldSystem *fs){
    int map = fs->location->mapId;
    MapObjectMan *man = fs->mapObjectMan;

    /* campo ricreato: gli oggetti non esistono piu', non rimuovere nulla */
    if (sObjMan != man || sFieldGone) {
        for (int i = 0; i < OWENC_SLOTS; i++) {
            OwEnc *e = &sOwEnc[i];
            e->obj = NULL;
            if (e->state == SLOT_BATTLE) OwEnc_Forget(e);   /* esito sconosciuto */
        }
        sObjMan = man; sFieldGone = 0;
        sPending = NULL; sPendingIdx = -1;
    }
    /* cambio mappa: i normali spariscono, gli shiny persistenti restano in memoria */
    if (sMap != map) {
        for (int i = 0; i < OWENC_SLOTS; i++) {
            OwEnc *e = &sOwEnc[i];
            if (e->state == SLOT_EMPTY) continue;
            if (e->keep) OwEnc_RemoveObj(e);
            else OwEnc_FreeSlot(e);
        }
        sMap = map;
    }
    /* aggiorna gli esistenti, ricrea quelli mancanti sulla mappa corrente */
    for (int i = 0; i < OWENC_SLOTS; i++) {
        OwEnc *e = &sOwEnc[i];
        if (e->state != SLOT_ACTIVE) continue;
        if (e->grace > 0) e->grace--;
        if (OwEnc_ObjValid(e)) {
            if (e->age < 0xFFFF) e->age++;
#if OWENC_NATIVE
            e->curX = (u16)MapObject_GetCurrentX(e->obj);
            e->curZ = (u16)MapObject_GetCurrentY(e->obj);   /* "Y" di hg-engine = Z */
#endif
            continue;
        }
        e->obj = NULL;
        if (e->mapId == map) OwEnc_BuildObject(fs, i);
    }
}

#if !OWENC_NATIVE
/* ===== wander manuale: rimuove e ricrea l'oggetto sulla nuova tile ===== */
static void OwEnc_Wander(FieldSystem *fs){
    int px = OwEnc_PlayerX(fs), pz = OwEnc_PlayerZ(fs);
    for (int i = 0; i < OWENC_SLOTS; i++) {
        OwEnc *e = &sOwEnc[i];
        if (e->state != SLOT_ACTIVE || !OwEnc_ObjValid(e)) continue;
        if (e->idle > 0) { e->idle--; continue; }
        if ((int)(gf_rand() % 100) >= OWENC_STEP_CHANCE) { e->idle = 2; continue; }
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
            || !OwEnc_IsWalkable(fs, nx, nz)) { e->idle = 4; continue; }
        /* ignora se stesso: sposta temporaneamente la posizione corrente fuori dal confronto */
        u16 ox = e->curX, oz = e->curZ;
        e->curX = 0xFFFF; e->curZ = 0xFFFF;
        BOOL busy = OwEnc_Occupied(nx, nz);
        e->curX = ox; e->curZ = oz;
        if (busy) { e->idle = 4; continue; }
        e->curX = (u16)nx; e->curZ = (u16)nz; e->dir = nd;
        OwEnc_RemoveObj(e);
        OwEnc_BuildObject(fs, i);       /* se fallisce, Sync riprova al prossimo passo */
        e->idle = 8;
    }
}
#endif

/* contatto: distanza di Chebyshev <= 1 (diagonali incluse) */
static BOOL OwEnc_TryContact(FieldSystem *fs){
    int px = OwEnc_PlayerX(fs), pz = OwEnc_PlayerZ(fs);
    for (int i = 0; i < OWENC_SLOTS; i++) {
        OwEnc *e = &sOwEnc[i];
        if (e->state != SLOT_ACTIVE || e->grace > 0) continue;
        if (e->mapId != fs->location->mapId || !OwEnc_ObjValid(e)) continue;
#if OWENC_NATIVE
        /* in nativo la posizione cambia tra un passo e l'altro: rileggila */
        e->curX = (u16)MapObject_GetCurrentX(e->obj);
        e->curZ = (u16)MapObject_GetCurrentY(e->obj);
#endif
        int dx = OwEnc_Abs(e->curX - px), dz = OwEnc_Abs(e->curZ - pz);
        if ((dx > dz ? dx : dz) <= 1) {
            if (OwEnc_StartContact(fs, i)) return TRUE;
        }
    }
    return FALSE;
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
    if (sCooldown > 0) sCooldown--;
    if (OwEnc_TryContact(fs)) return TRUE;         /* prima del wander */
#if !OWENC_NATIVE
    OwEnc_Wander(fs);
    if (OwEnc_TryContact(fs)) return TRUE;         /* dopo il wander */
#endif
    if (!Ow_HasWildEncounters(fs->location->mapId)) return FALSE;
    sStarted = 0; sInCheck = 1;
    Ow_Perform(fs);
    sInCheck = 0;
    return sStarted;
}