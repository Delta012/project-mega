#include "types.h"
#include "pokemon.h"
#include "battle.h"
#include "map_events_internal.h"

void LONG_CALL sub_02050B08(FieldSystem *fs, struct BattleSetup *setup);
void LONG_CALL BattleSetup_Delete(struct BattleSetup *setup);
u8   LONG_CALL GetMetatileBehavior(FieldSystem *fs, int x, int z);
int  LONG_CALL PlayerAvatar_GetXCoord(FIELD_PLAYER_AVATAR *avatar);
int  LONG_CALL PlayerAvatar_GetZCoord(FIELD_PLAYER_AVATAR *avatar);
u8   LONG_CALL GetMonGender(struct PartyPokemon *mon);
BOOL LONG_CALL FieldSystem_PerformLandOrSurfEncounterCheck(FieldSystem *fs);
u8   LONG_CALL FieldSystem_GetTileEncounterTypeAndRate(FieldSystem *fs, u8 metatileBehavior, u8 *typeOut);

void OwEnc_Capture(FieldSystem *fs, struct BattleSetup *setup);
BOOL OwEnc_OnStep(FieldSystem *fs);

void OwEnc_OnStep_Tramp(void);
void OwEnc_Capture_Tramp(void);