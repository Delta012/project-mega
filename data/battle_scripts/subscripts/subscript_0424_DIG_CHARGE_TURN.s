#include "constants/battle_constants.h"
.include "battle_commands.inc"

.data

_000:
    UpdateMonData OPCODE_FLAG_ON, BATTLER_CATEGORY_ATTACKER, BMON_DATA_MOVE_EFFECT, MOVE_EFFECT_FLAG_DIG
    PlayMoveAnimation BATTLER_CATEGORY_ATTACKER
    Wait 
    // {0} burrowed its way under the ground!
    PrintMessage 226, TAG_NICKNAME, BATTLER_CATEGORY_ATTACKER 
    Wait 
    WaitButtonABTime 30
    LockMoveChoice BATTLER_CATEGORY_ATTACKER
    CompareMonDataToValue OPCODE_FLAG_NOT, BATTLER_CATEGORY_ATTACKER, BMON_DATA_MOVE_EFFECT, MOVE_EFFECT_FLAG_SEMI_INVULNERABLE, _017
    ToggleVanish BATTLER_CATEGORY_ATTACKER, TRUE

_017:
    End 
