#include "config.h"
#include "types.h"

#include "rtc.h"
#include "save.h"
#include "system.h"

// variable where the clock is stored
#define GAME_CLOCK_ELAPSED_SECONDS_VAR 0x40E4

#define SECONDS_PER_DAY 86400

#define MORNING_SECONDS (GAME_CLOCK_MORNING_MINUTES * 60)
#define DAY_SECONDS     (GAME_CLOCK_DAY_MINUTES * 60)
#define EVENING_SECONDS (GAME_CLOCK_EVENING_MINUTES * 60)
#define NIGHT_SECONDS   (GAME_CLOCK_NIGHT_MINUTES * 60)

#define COMPLETE_CYCLES_SECONDS (MORNING_SECONDS + DAY_SECONDS + EVENING_SECONDS + NIGHT_SECONDS)

// Script to add detached in-game clock from real time, as latest gen games. Dates, weekdays and seasons still follow the real calendar.
// The in-game clock is only advancing while the game is running, and it's stored in the save file.

enum DailyClockCycles {
    CLOCK_CYCLE_MORNING,
    CLOCK_CYCLE_DAY,
    CLOCK_CYCLE_EVENING,
    CLOCK_CYCLE_NIGHT,
    CLOCK_CYCLE_COUNT,
};

// A bad value in config.h fails the build instead of breaking the in-game clock
#define CLOCK_STATIC_ASSERT(name, condition) typedef char name[(condition) ? 1 : -1]

CLOCK_STATIC_ASSERT(ClockCyclesNotEmpty, MORNING_SECONDS > 0 && DAY_SECONDS > 0 && EVENING_SECONDS > 0 && NIGHT_SECONDS > 0);
CLOCK_STATIC_ASSERT(ClockNewGameCycleValid, GAME_CLOCK_NEW_GAME_CYCLE >= 0 && GAME_CLOCK_NEW_GAME_CYCLE < CLOCK_CYCLE_COUNT);
CLOCK_STATIC_ASSERT(ClockCycleNoOverflow, COMPLETE_CYCLES_SECONDS < 100000);
CLOCK_STATIC_ASSERT(ClockCycleFitsInVar, COMPLETE_CYCLES_SECONDS <= 0xFFFF);

struct ClockCycle {
    u32 CycleSeconds;           // How many seconds a cycle lasts
    u32 CycleStartSecond;       // At which in-game second a cycle starts at
    u32 CycleInGameSeconds;     // How many in-game seconds a cycle lasts
};

// Makes sure the in-game time does match with day tables, so the tint, music and encounters are correctly changed with the new cycles
static const struct ClockCycle sClockCycles[CLOCK_CYCLE_COUNT] = {
    [CLOCK_CYCLE_MORNING] = { MORNING_SECONDS,  4 * 3600,  6 * 3600 }, // 04:00 - 10:00
    [CLOCK_CYCLE_DAY]     = { DAY_SECONDS,     10 * 3600,  7 * 3600 }, // 10:00 - 17:00
    [CLOCK_CYCLE_EVENING] = { EVENING_SECONDS, 17 * 3600,  3 * 3600 }, // 17:00 - 20:00
    [CLOCK_CYCLE_NIGHT]   = { NIGHT_SECONDS,   20 * 3600,  8 * 3600 }, // 20:00 - 04:00
};

// NDS runs at ~59.83. The in-game clock runs ~0.3% slower, which is not noticeable at all
#define FRAMES_PER_SECOND 60

static u32 sLastVblank;
static u32 sFrameRemainder;
static BOOL sTickReady;

// Real seconds elapsed in a complete cycle, counted from a new game
static u32 GetCycleElapsedSeconds(void)
{
    u32 elapsedSeconds;
    u32 now = gSystem.vblankCounter;

    if (SaveBlock2_get() == NULL) {
        return 0;
    }

    elapsedSeconds = GetScriptVar(GAME_CLOCK_ELAPSED_SECONDS_VAR) % COMPLETE_CYCLES_SECONDS;

    if (sTickReady && now > sLastVblank) {
        u32 seconds;

        sFrameRemainder += now - sLastVblank;
        seconds = sFrameRemainder / FRAMES_PER_SECOND;
        sFrameRemainder %= FRAMES_PER_SECOND;

        if (seconds != 0) {
            elapsedSeconds = (elapsedSeconds + seconds) % COMPLETE_CYCLES_SECONDS;
            SetScriptVar(GAME_CLOCK_ELAPSED_SECONDS_VAR, elapsedSeconds);
        }
    }

    sLastVblank = now;
    sTickReady = TRUE;

    return elapsedSeconds;
}

// In-game second of a day (0-86399)
static u32 GetGameSecondOfDay(void)
{
    u32 elapsedSeconds = GetCycleElapsedSeconds();
    int i;

    for (i = 0; i < GAME_CLOCK_NEW_GAME_CYCLE; i++) {
        elapsedSeconds += sClockCycles[i].CycleSeconds;
    }

    elapsedSeconds %= COMPLETE_CYCLES_SECONDS;

    for (i = 0; i < CLOCK_CYCLE_COUNT; i++) {
        const struct ClockCycle *cycle = &sClockCycles[i];

        if (elapsedSeconds < cycle->CycleSeconds) {
            return (cycle->CycleStartSecond + elapsedSeconds * cycle->CycleInGameSeconds / cycle->CycleSeconds) % SECONDS_PER_DAY;
        }

        elapsedSeconds -= cycle->CycleSeconds;
    }

    return 0;
}

// Function to hook encounters, evolutions, music and gracidea
void LONG_CALL GF_RTC_CopyTime_Detached(struct RTCTime *time)
{
    u32 gameSecondOfDay = GetGameSecondOfDay();

    time->hour = gameSecondOfDay / 3600;
    time->minute = (gameSecondOfDay % 3600) / 60;
    time->second = gameSecondOfDay % 60;
}

// Function to hook environment tints
s32 LONG_CALL GF_RTC_TimeToSec_Detached(void)
{
    return (s32)GetGameSecondOfDay();
}

// Function to hook battle backgrounds, GetTimeOfDay command and npc trades
TIMEOFDAY LONG_CALL Field_GetTimeOfDay_Detached(void *fieldSystem)
{
    (void)fieldSystem;
    return GF_RTC_GetTimeOfDay();
}

// Function to hook npc rematches
TimeOfDayWildParam LONG_CALL Field_GetTimeOfDayWildParam_Detached(void *fieldSystem)
{
    (void)fieldSystem;
    return GF_RTC_GetTimeOfDayWildParam();
}

// Function to get the exact hour
u32 LONG_CALL Field_GetHour_Detached(void *fieldSystem)
{
    (void)fieldSystem;
    return GetGameSecondOfDay() / 3600;
}