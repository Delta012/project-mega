#include "constants/battle_constants.h"
.include "battle_commands.inc"

.data

_000:
    CompareVarToValue OPCODE_FLAG_SET, BSCRIPT_VAR_FIELD_CONDITION, FIELD_CONDITION_RAIN_PERMANENT|FIELD_CONDITION_RAIN, _HostileWeatherActive
    GoToEffectScript 

_HostileWeatherActive:
    // But it failed!
    PrintMessage 796, TAG_NONE
    Wait
    WaitButtonABTime 30
    End