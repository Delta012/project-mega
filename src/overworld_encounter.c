#include "overworld_encounter.h"
#include "types.h"

#define OWENC_MAX      3
#define OWENC_OBJ_ID   0xE0
#define OWENC_RADIUS   4
#define OWENC_VISIBLE  MAPOBJECTFLAG_UNK9      // decomp: MAPOBJECTFLAG_VISIBLE (1 << 9)
#define MAP_NOTHING_ID 1

typedef struct {
    struct BattleSetup *setup;
    LocalMapObject *obj;
    u8 active;
} OwEnc;

// Forziamo .data: i static a zero finiscono in .bss, che potrebbe non essere azzerato
// dopo un soft reset nell'overlay sintetico.
static OwEnc sOwEnc[OWENC_MAX] __attribute__((section(".data"))) = { {NULL, NULL, 0}, {NULL, NULL, 0}, {NULL, NULL, 0} };
static int sMap __attribute__((section(".data"))) = -1;
static FieldSystem *sFs __attribute__((section(".data"))) = NULL;

static void OwEnc_FreeSlot(OwEnc *e, BOOL freeSetup) {
    if (e->obj) DeleteMapObject(e->obj);
    if (freeSetup && e->setup) BattleSetup_Delete(e->setup);
    e->obj = NULL; e->setup = NULL; e->active = 0;
}

static BOOL OwEnc_PickSpawnTile(FieldSystem *fs, int *outX, int *outZ) {
    int px = PlayerAvatar_GetXCoord(fs->playerAvatar);
    int pz = PlayerAvatar_GetZCoord(fs->playerAvatar);
    for (int tries = 0; tries < 12; tries++) {
        int dx = (int)(gf_rand() % (OWENC_RADIUS * 2 + 1)) - OWENC_RADIUS;
        int dz = (int)(gf_rand() % (OWENC_RADIUS * 2 + 1)) - OWENC_RADIUS;
        if ((dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz) < 2) continue;   // non troppo vicino
        u8 type;
        u8 beh = GetMetatileBehavior(fs, px + dx, pz + dz);
        if (FieldSystem_GetTileEncounterTypeAndRate(fs, beh, &type) == 0) continue;  // solo tile con incontri
        *outX = px + dx; *outZ = pz + dz;
        return TRUE;
    }
    return FALSE;
}

static BOOL OwEnc_Touching(FieldSystem *fs, LocalMapObject *o) {
    int dx = PlayerAvatar_GetXCoord(fs->playerAvatar) - (int)MapObject_GetCurrentX(o);
    int dz = PlayerAvatar_GetZCoord(fs->playerAvatar) - (int)MapObject_GetCurrentY(o);  // "Y" di hg-engine = Z
    if (dx < 0) dx = -dx;
    if (dz < 0) dz = -dz;
    return dx + dz <= 1;
}

void OwEnc_Capture(FieldSystem *fs, struct BattleSetup *setup) {
    int i;
    for (i = 0; i < OWENC_MAX && sOwEnc[i].active; i++);
    if (i == OWENC_MAX) { BattleSetup_Delete(setup); return; }

    struct PartyPokemon *mon = Party_GetMonByIndex(setup->party[1], 0);   // BATTLER_ENEMY = 1
    int species = GetMonData(mon, MON_DATA_SPECIES, 0);
    int x, z;
    if (species > 493 || !OwEnc_PickSpawnTile(fs, &x, &z)) { BattleSetup_Delete(setup); return; }
    int  form   = GetMonData(mon, MON_DATA_FORM, 0);
    int  gender = GetMonGender(mon);
    BOOL shiny  = MonIsShiny(mon);

    LocalMapObject *o = CreateSpecialFieldObject(fs->mapObjectMan, x, z, 1,
                            FollowingPokemon_GetSpriteID(species, form, gender),
                            0, MAP_NOTHING_ID);
    if (!o) { BattleSetup_Delete(setup); return; }
    MapObject_SetID(o, OWENC_OBJ_ID + i);
    MapObject_SetType(o, 0);
    MapObject_SetFlagID(o, 0);
    MapObject_SetXRange(o, -1);
    MapObject_SetYRange(o, -1);
    FollowPokeMapObjectSetParams(o, species, form, shiny);
    MapObject_SetFlag29(o, TRUE);
    MapObject_SetBits(o, OWENC_VISIBLE);

    sOwEnc[i].setup = setup; sOwEnc[i].obj = o; sOwEnc[i].active = 1;
}

BOOL OwEnc_OnStep(FieldSystem *fs) {
    if (sFs != fs) {                    // nuovo FieldSystem (load/reset): l'heap è sparito, niente free
        for (int i = 0; i < OWENC_MAX; i++) OwEnc_FreeSlot(&sOwEnc[i], FALSE);
        sFs = fs; sMap = fs->location->mapId;
    } else if (sMap != fs->location->mapId) {
        for (int i = 0; i < OWENC_MAX; i++) if (sOwEnc[i].active) OwEnc_FreeSlot(&sOwEnc[i], TRUE);
        sMap = fs->location->mapId;
    }
    for (int i = 0; i < OWENC_MAX; i++) {
        OwEnc *e = &sOwEnc[i];
        if (e->active && OwEnc_Touching(fs, e->obj)) {
            struct BattleSetup *s = e->setup;
            DeleteMapObject(e->obj);
            e->obj = NULL; e->setup = NULL; e->active = 0;
            sub_02050B08(fs, s);        // battaglia vera con il mon già generato (shiny incluso)
            return TRUE;
        }
    }
    FieldSystem_PerformLandOrSurfEncounterCheck(fs);   // può chiamare OwEnc_Capture
    return FALSE;
}

// Trampoline hook 1 @ 0x021E7180. On entry r0 = fs (r4). Redoes: cmp r0,#1 / bne 0x021E718E.
__attribute__((naked, used)) void OwEnc_OnStep_Tramp(void) {
    __asm__ volatile(
        "bl OwEnc_OnStep\n"
        "cmp r0, #1\n"
        "bne 1f\n"
        "ldr r1, =0x021E7189\n"     // 0x021E7188 | thumb
        "bx r1\n"
        "1:\n"
        "ldr r1, =0x021E718F\n"     // 0x021E718E | thumb
        "bx r1\n"
        ".ltorg\n"
    );
}

// Trampoline hook 2 @ 0x02246F2E. On entry r0 = fs (r5), r1 = setup.
// Redoes: adds r0,r5,#0 / movs r1,#0 / adds r0,#126, then resumes at 0x02246F38.
__attribute__((naked, used)) void OwEnc_Capture_Tramp(void) {
    __asm__ volatile(
        "bl OwEnc_Capture\n"
        "adds r0, r5, #0\n"
        "movs r1, #0\n"
        "adds r0, #126\n"
        "ldr r2, =0x02246F39\n"     // 0x02246F38 | thumb
        "bx r2\n"
        ".ltorg\n"
    );
}