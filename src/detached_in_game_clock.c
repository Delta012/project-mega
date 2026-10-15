#include "config.h"
#include "types.h"

#include "rtc.h"
#include "save.h"
#include "system.h"

// Detached in-game clock: the time of day follows the play time instead of the real clock, like in the latest generations.
// Dates, weekdays and seasons still follow the real calendar. The clock only advances while the game is running and it is stored in the save.
// Apricorn trees and Berry Pots grow with the same clock (see config.h for BERRY_REGROW_DAYS and APRICORN_REGROW_DAYS).

// Save variables. None of them is referenced by the vanilla scripts.
#define GAME_CLOCK_ELAPSED_SECONDS_VAR 0x40E4                               // seconds of play inside the current cycle
#define GAME_CLOCK_VARS_BASE           0x4059
#define GAME_CLOCK_APRICORN_DAYS_VAR   GAME_CLOCK_VARS_BASE                 // in-game days since the apricorn trees regrew
#define GAME_CLOCK_BERRY_CLOCK_VAR     (GAME_CLOCK_VARS_BASE + 1)           // 2 vars: seconds of play since the new game
#define GAME_CLOCK_BERRY_STAMP_VAR     (GAME_CLOCK_VARS_BASE + 3)           // 2 vars per pot: berry clock at the pot's last update
#define GAME_CLOCK_VARS_END            (GAME_CLOCK_BERRY_STAMP_VAR + MAX_BERRY_POT * 2)

#define SECONDS_PER_DAY 86400

#define MORNING_SECONDS (GAME_CLOCK_MORNING_MINUTES * 60)
#define DAY_SECONDS     (GAME_CLOCK_DAY_MINUTES * 60)
#define EVENING_SECONDS (GAME_CLOCK_EVENING_MINUTES * 60)
#define NIGHT_SECONDS   (GAME_CLOCK_NIGHT_MINUTES * 60)

#define COMPLETE_CYCLES_SECONDS (MORNING_SECONDS + DAY_SECONDS + EVENING_SECONDS + NIGHT_SECONDS)

// Seconds of play before the cycle the new game starts at
#define NEW_GAME_OFFSET_SECONDS ((GAME_CLOCK_NEW_GAME_CYCLE > 0 ? MORNING_SECONDS : 0) \
                               + (GAME_CLOCK_NEW_GAME_CYCLE > 1 ? DAY_SECONDS : 0)     \
                               + (GAME_CLOCK_NEW_GAME_CYCLE > 2 ? EVENING_SECONDS : 0))

// The vanilla growth time of a berry is its hours * 60 minutes
#define BERRY_MINUTES_PER_HOUR 60

// NDS runs at ~59.83. The in-game clock runs ~0.3% slower, which is not noticeable at all
#define FRAMES_PER_SECOND 60

enum DailyClockCycles {
    CLOCK_CYCLE_MORNING,
    CLOCK_CYCLE_DAY,
    CLOCK_CYCLE_EVENING,
    CLOCK_CYCLE_NIGHT,
    CLOCK_CYCLE_COUNT,
};

// A bad value in config.h fails the build instead of breaking the game
#define CLOCK_STATIC_ASSERT(name, condition) typedef char name[(condition) ? 1 : -1]

CLOCK_STATIC_ASSERT(ClockCyclesNotEmpty, MORNING_SECONDS > 0 && DAY_SECONDS > 0 && EVENING_SECONDS > 0 && NIGHT_SECONDS > 0);
CLOCK_STATIC_ASSERT(ClockNewGameCycleValid, GAME_CLOCK_NEW_GAME_CYCLE >= 0 && GAME_CLOCK_NEW_GAME_CYCLE < CLOCK_CYCLE_COUNT);
CLOCK_STATIC_ASSERT(ClockCycleFitsInVar, COMPLETE_CYCLES_SECONDS <= 0xFFFF);
CLOCK_STATIC_ASSERT(ClockVarsInSaveRange, GAME_CLOCK_VARS_BASE >= 0x4020 && GAME_CLOCK_VARS_END - 1 <= 0x416F);
CLOCK_STATIC_ASSERT(ClockVarsDistinct, GAME_CLOCK_ELAPSED_SECONDS_VAR < GAME_CLOCK_VARS_BASE || GAME_CLOCK_ELAPSED_SECONDS_VAR >= GAME_CLOCK_VARS_END);
CLOCK_STATIC_ASSERT(ApricornRegrowDaysValid, APRICORN_REGROW_DAYS >= 1 && APRICORN_REGROW_DAYS <= 0xFFFF);
CLOCK_STATIC_ASSERT(BerryRegrowDaysValid, BERRY_REGROW_DAYS >= 1);

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

static u32 sLastVblank;
static u32 sFrameRemainder;
static u32 sLastGameSecond = SECONDS_PER_DAY; // SECONDS_PER_DAY = no second seen yet
static BOOL sTickReady;

static u32 GetVar32(u16 var)
{
    return GetScriptVar(var) | ((u32)GetScriptVar(var + 1) << 16);
}

static void SetVar32(u16 var, u32 value)
{
    SetScriptVar(var, value & 0xFFFF);
    SetScriptVar(var + 1, value >> 16);
}

// Seconds of play elapsed in the current cycle, counted from a new game
static u32 GetCycleElapsedSeconds(void)
{
    u32 now = gSystem.vblankCounter;
    u32 elapsedSeconds;

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
            SetVar32(GAME_CLOCK_BERRY_CLOCK_VAR, GetVar32(GAME_CLOCK_BERRY_CLOCK_VAR) + seconds);
        }
    }

    sLastVblank = now;
    sTickReady = TRUE;

    return elapsedSeconds;
}

// Same as the vanilla sub_0202AE74: every picked tree grows its apricorns back
static void RegrowApricornTrees(void)
{
    struct SAVE_MISC_DATA *misc = Sav2_Misc_get(SaveBlock2_get());
    int i;

    for (i = 0; i < MAX_APRICORN_TREE; i++) {
        if (misc->apricorn_trees[i].unk_0 == 0) {
            misc->apricorn_trees[i].unk_0 = 1;
            misc->apricorn_trees[i].unk_1 = 1;
        }
    }
}

// Apricorn trees regrow every APRICORN_REGROW_DAYS in-game days instead of once per real day
static void UpdateApricornTrees(u32 gameSecond)
{
    if (sLastGameSecond != SECONDS_PER_DAY && gameSecond < sLastGameSecond) {
        // in-game midnight passed: a full in-game day is over
        u32 days = GetScriptVar(GAME_CLOCK_APRICORN_DAYS_VAR) + 1;

        if (days >= APRICORN_REGROW_DAYS) {
            RegrowApricornTrees();
            days = 0;
        }

        SetScriptVar(GAME_CLOCK_APRICORN_DAYS_VAR, days);
    }

    sLastGameSecond = gameSecond;
}

// In-game second of a day (0-86399)
static u32 GetGameSecondOfDay(void)
{
    u32 elapsedSeconds = (GetCycleElapsedSeconds() + NEW_GAME_OFFSET_SECONDS) % COMPLETE_CYCLES_SECONDS;
    u32 gameSecond = 0;
    int i;

    for (i = 0; i < CLOCK_CYCLE_COUNT; i++) {
        const struct ClockCycle *cycle = &sClockCycles[i];

        if (elapsedSeconds < cycle->CycleSeconds) {
            gameSecond = (cycle->CycleStartSecond + elapsedSeconds * cycle->CycleInGameSeconds / cycle->CycleSeconds) % SECONDS_PER_DAY;
            break;
        }

        elapsedSeconds -= cycle->CycleSeconds;
    }

    if (SaveBlock2_get() != NULL) {
        UpdateApricornTrees(gameSecond);
    }

    return gameSecond;
}

// Function to hook encounters, evolutions, music and gracidea
void LONG_CALL GF_RTC_CopyTime_Detached(struct RTCTime *time)
{
    u32 gameSecond = GetGameSecondOfDay();

    time->hour = gameSecond / 3600;
    time->minute = (gameSecond % 3600) / 60;
    time->second = gameSecond % 60;
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

// Function to hook the vanilla daily apricorn regrow (sub_020556B8): it does nothing, so real days no longer regrow the trees
void LONG_CALL Field_RegrowApricornTrees_Detached(void *fieldSystem)
{
    (void)fieldSystem;
}

// Berry Pots: the vanilla update grows the pots with the real minutes passed since the last visit.
// Here a pot grows with its own play time instead. A berry whose vanilla growth time is H hours needs
// BERRY_REGROW_DAYS in-game days * (1 + H / 100) of play per growth stage. Example with 4 days: H = 36 -> 4 days * 1.36.

// Overlay functions: ov16_02201760 grows the pots by the given vanilla minutes, the others redraw the app
#define ov16_02201760 ((void (*)(BerryPot *, const void *, s32))(0x02201760u | 1u))
#define ov17_0220387C ((void (*)(void *))(0x0220387Cu | 1u))
#define ov17_02203928 ((void (*)(void *))(0x02203928u | 1u))

// First fields of BerryPotsAppData (offsets 0x00 - 0x1F)
struct BerryPotsAppDataHead {
    u32 unused[5];
    BerryPot *berryPots;
    void *unusedDatetime;
    void *berryGrowthProperties;
};

// Per berry data (UnkStruct_ov16_022014A0, size 0x3), indexed by berryId - 1
struct BerryGrowthProperties {
    u8 growthHours;
    u8 drainRate;
    u8 unk2;
};

// Function to hook the Berry Pots growth update (ov17_022037C8)
void LONG_CALL BerryPots_UpdateGrowth_Detached(struct BerryPotsAppDataHead *data, BOOL a1)
{
    const struct BerryGrowthProperties *props = (const struct BerryGrowthProperties *)data->berryGrowthProperties;
    u32 berryClock;
    BOOL anyGrown = FALSE;
    int i;

    (void)GetCycleElapsedSeconds();   // count the frames since the last clock read
    berryClock = GetVar32(GAME_CLOCK_BERRY_CLOCK_VAR);

    for (i = 0; i < MAX_BERRY_POT; i++) {
        BerryPot *pot = &data->berryPots[i];
        u16 stampVar = GAME_CLOCK_BERRY_STAMP_VAR + i * 2;
        u32 hours;
        u32 rate;
        u32 secondsPerStage;
        u32 stamp;
        u32 pending;
        u32 minutes;

        if (pot->berryId == 0) {      // empty pot: its play time starts counting when it is planted
            SetVar32(stampVar, berryClock);
            continue;
        }

        hours = props[pot->berryId - 1].growthHours;
        rate = hours * BERRY_MINUTES_PER_HOUR;
        if (rate == 0) {
            continue;
        }

        // seconds of play one growth stage takes
        secondsPerStage = BERRY_REGROW_DAYS * COMPLETE_CYCLES_SECONDS * (100 + hours) / 100;

        // play time of this pot, limited so the multiplications below stay inside 32 bits
        stamp = GetVar32(stampVar);
        pending = berryClock - stamp;
        if (pending > (0xFFFFFFFF - rate) / rate) {
            pending = (0xFFFFFFFF - rate) / rate;
        }

        // vanilla minutes of growth earned by that play time
        minutes = pending * rate / secondsPerStage;
        if (minutes == 0) {
            continue;
        }

        // only the seconds those minutes are worth are used, the leftover stays for the next update
        SetVar32(stampVar, stamp + (minutes * secondsPerStage + rate - 1) / rate);

        {
            // the vanilla function loops over 4 pots, so it gets this pot alone (berryId 0 = empty = skipped)
            BerryPot single[MAX_BERRY_POT];
            int k;

            single[0] = *pot;
            for (k = 1; k < MAX_BERRY_POT; k++) {
                single[k].berryId = 0;
            }
            ov16_02201760(single, props, minutes);
            *pot = single[0];
        }

        anyGrown = TRUE;
    }

    if (anyGrown) {
        ov17_0220387C(data);
        if (a1) {
            ov17_02203928(data);
        }
    }
}