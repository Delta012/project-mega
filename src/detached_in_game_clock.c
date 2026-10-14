#include "config.h"
#include "types.h"

#include "rtc.h"
#include "save.h"
#include "system.h"

// variables where to store stuff
#define GAME_CLOCK_ELAPSED_SECONDS_VAR 0x40E4
#define GAME_CLOCK_APRICORN_DAYS_VAR 0x40E3
#define GAME_CLOCK_BERRY_DAYS_VAR 0x40E5
#define BERRY_VARS_COUNT (MAX_BERRY_POT * 2)

#define SECONDS_PER_DAY 86400

#define MORNING_SECONDS (GAME_CLOCK_MORNING_MINUTES * 60)
#define DAY_SECONDS     (GAME_CLOCK_DAY_MINUTES * 60)
#define EVENING_SECONDS (GAME_CLOCK_EVENING_MINUTES * 60)
#define NIGHT_SECONDS   (GAME_CLOCK_NIGHT_MINUTES * 60)

#define COMPLETE_CYCLES_SECONDS (MORNING_SECONDS + DAY_SECONDS + EVENING_SECONDS + NIGHT_SECONDS)

static u32 sLastVblank;
static u32 sFrameRemainder;
static u32 sGameSecond = 0;
static BOOL sTickReady;
static u32 GetCycleElapsedSeconds(void);
static u32 GetGameSecondOfDay(void);

// berry growth hooks
#define ov16_02201760 ((void (*)(BerryPot *, void *, s32))(0x02201760u | 1u))
#define ov17_0220387C ((void (*)(void *))(0x0220387Cu | 1u))
#define ov17_02203928 ((void (*)(void *))(0x02203928u | 1u))

// Berry growth (BERRY_BASE_DAYS is set in config.h):
// a berry whose vanilla growth time is H hours needs BERRY_BASE_DAYS in-game days * (1 + H / 100) per growth stage.
// One in-game day = one complete cycle of play. Example with 4 days and 100: H = 36 -> 4 days * 1.36.
#define BERRY_CHUNK_SECONDS    65535     // seconds converted per step, so the multiplication stays inside 32 bits
#define BERRY_MAX_MINUTES      4000000   // more than the whole life of any berry (the vanilla code clears the plant beyond it)

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
CLOCK_STATIC_ASSERT(BerryVarsInSaveRange, GAME_CLOCK_BERRY_DAYS_VAR >= 0x4020 && GAME_CLOCK_BERRY_DAYS_VAR + MAX_BERRY_POT - 1 <= 0x416F);
CLOCK_STATIC_ASSERT(BerryVarsDistinct, (GAME_CLOCK_ELAPSED_SECONDS_VAR < GAME_CLOCK_BERRY_DAYS_VAR || GAME_CLOCK_ELAPSED_SECONDS_VAR > GAME_CLOCK_BERRY_DAYS_VAR + MAX_BERRY_POT - 1)
                                       && (GAME_CLOCK_APRICORN_DAYS_VAR < GAME_CLOCK_BERRY_DAYS_VAR || GAME_CLOCK_APRICORN_DAYS_VAR > GAME_CLOCK_BERRY_DAYS_VAR + MAX_BERRY_POT - 1));
CLOCK_STATIC_ASSERT(BerryBaseDaysValid, BERRY_BASE_DAYS >= 1);

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

// Seconds of play not yet applied to each Berry Pot (one var per pot, stops counting at 65535)
static u32 GetPotSeconds(int pot)
{
    u32 low = GetScriptVar(GAME_CLOCK_BERRY_DAYS_VAR + pot * 2);
    u32 high = GetScriptVar(GAME_CLOCK_BERRY_DAYS_VAR + pot * 2 + 1);

    return low | (high << 16);
}

static void SetPotSeconds(int pot, u32 value)
{
    SetScriptVar(GAME_CLOCK_BERRY_DAYS_VAR + pot * 2, value & 0xFFFF);
    SetScriptVar(GAME_CLOCK_BERRY_DAYS_VAR + pot * 2 + 1, value >> 16);
}

static void AddBerrySeconds(u32 seconds)
{
    int i;

    for (i = 0; i < MAX_BERRY_POT; i++) {
        u32 total = GetPotSeconds(i) + seconds;

        if (total < seconds) {   // overflow: stay at the maximum
            total = 0xFFFFFFFF;
        }
        SetPotSeconds(i, total);
    }
}

// Apricorn trees regrow once per x in-game time, where x is configured by editing GAME_CLOCK_APRICORN_REGROW_DAYS in config.h, instead of once per real day.
#define APRICORN_NO_SECOND_SEEN SECONDS_PER_DAY

CLOCK_STATIC_ASSERT(ApricornRegrowDaysValid, GAME_CLOCK_APRICORN_REGROW_DAYS >= 1 && GAME_CLOCK_APRICORN_REGROW_DAYS <= 0xFFFF);
CLOCK_STATIC_ASSERT(ApricornVarInSaveRange, GAME_CLOCK_APRICORN_DAYS_VAR >= 0x4020 && GAME_CLOCK_APRICORN_DAYS_VAR <= 0x416F);
CLOCK_STATIC_ASSERT(ApricornVarNotClockVar, GAME_CLOCK_APRICORN_DAYS_VAR != GAME_CLOCK_ELAPSED_SECONDS_VAR);

static u32 sLastGameSecond = APRICORN_NO_SECOND_SEEN;

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

static void UpdateApricornTrees(u32 sGameSecond)
{
    if (sLastGameSecond != APRICORN_NO_SECOND_SEEN && sGameSecond < sLastGameSecond) {
        // in-game midnight passed: a full in-game day (one complete cycle) is over
        u32 days = GetScriptVar(GAME_CLOCK_APRICORN_DAYS_VAR) + 1;

        if (days >= GAME_CLOCK_APRICORN_REGROW_DAYS) {
            RegrowApricornTrees();
            days = 0;
        }

        SetScriptVar(GAME_CLOCK_APRICORN_DAYS_VAR, days);
    }

    sLastGameSecond = sGameSecond;
}

// Function to hook encounters, evolutions, music and gracidea
void LONG_CALL GF_RTC_CopyTime_Detached(struct RTCTime *time)
{
    u32 tempGameSecond = GetGameSecondOfDay();

    time->hour = tempGameSecond / 3600;
    time->minute = (tempGameSecond % 3600) / 60;
    time->second = tempGameSecond % 60;
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

// First fields of BerryPotsAppData (offsets 0x00 - 0x1F)
struct BerryPotsAppDataHead {
    u32 heapID;
    void *args;
    void *unk8;
    void *bag;
    void *stats;
    BerryPot *berryPots;
    struct GF_RTC_DateTime *berryDatetime;
    void *berryGrowthProperties;
};

// Per berry data (UnkStruct_ov16_022014A0, size 0x3), indexed by berryId - 1. Check the field order in include/overlay_16.h
struct BerryGrowthProperties {
    u8 growthHours;     // vanilla growth time of a stage, in hours
    u8 drainRate;
    u8 unk2;
};

// Function to hook the Berry Pots growth update (ov17_022037C8): each pot grows with the play time instead of the real clock
void LONG_CALL BerryPots_UpdateGrowth_Detached(struct BerryPotsAppDataHead *data, BOOL a1)
{
    struct BerryGrowthProperties *props = (struct BerryGrowthProperties *)data->berryGrowthProperties;
    BOOL anyGrown = FALSE;
    int i;

    (void)GetCycleElapsedSeconds();   // count the frames since the last clock read

    for (i = 0; i < MAX_BERRY_POT; i++) {
        BerryPot *pot = &data->berryPots[i];
        u32 hours;
        u32 secondsPerStage;
        u32 pending;
        u32 minutes;

        if (pot->berryId == 0) {      // empty pot: nothing to grow, so nothing is counted
            SetPotSeconds(i, 0);
            continue;
        }

        hours = props[pot->berryId - 1].growthHours;
        if (hours == 0) {
            continue;
        }

        // seconds of play one growth stage takes: base days * (1 + hours / divisor)
        secondsPerStage = BERRY_BASE_DAYS * COMPLETE_CYCLES_SECONDS * (1 + hours) / 100;

        // vanilla minutes of growth earned by the play time of this pot
        pending = GetPotSeconds(i);
        minutes = 0;
        while (pending != 0 && minutes < BERRY_MAX_MINUTES) {
            u32 chunk = pending > BERRY_CHUNK_SECONDS ? BERRY_CHUNK_SECONDS : pending;
            u32 stepMinutes = chunk * (hours * BERRY_MINUTES_PER_HOUR) / secondsPerStage;
            u32 usedSeconds;

            if (stepMinutes == 0) {
                break;
            }

            // only the seconds those minutes are worth are used, the leftover stays for the next update
            usedSeconds = stepMinutes * secondsPerStage / (hours * BERRY_MINUTES_PER_HOUR);
            if (usedSeconds == 0) {
                usedSeconds = 1;
            }
            pending -= usedSeconds;
            minutes += stepMinutes;
        }

        if (minutes == 0) {
            continue;
        }
        
        SetPotSeconds(i, pending);

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

// Real seconds elapsed in a complete cycle, counted from a new game
static u32 GetCycleElapsedSeconds(void)
{
    u32 sElapsedSeconds;
    u32 now = gSystem.vblankCounter;

    if (SaveBlock2_get() == NULL) {
        return 0;
    }

    sElapsedSeconds = GetScriptVar(GAME_CLOCK_ELAPSED_SECONDS_VAR) % COMPLETE_CYCLES_SECONDS;

    if (sTickReady && now > sLastVblank) {
        u32 seconds;

        sFrameRemainder += now - sLastVblank;
        seconds = sFrameRemainder / FRAMES_PER_SECOND;
        sFrameRemainder %= FRAMES_PER_SECOND;

        if (seconds != 0) {
            sElapsedSeconds = (sElapsedSeconds + seconds) % COMPLETE_CYCLES_SECONDS;
            SetScriptVar(GAME_CLOCK_ELAPSED_SECONDS_VAR, sElapsedSeconds);
            AddBerrySeconds(seconds);
        }
    }

    sLastVblank = now;
    sTickReady = TRUE;

    return sElapsedSeconds;
}

// In-game second of a day (0-86399)
static u32 GetGameSecondOfDay(void)
{
    u32 sElapsedSeconds = GetCycleElapsedSeconds();
    int i;

    for (i = 0; i < GAME_CLOCK_NEW_GAME_CYCLE; i++) {
        sElapsedSeconds += sClockCycles[i].CycleSeconds;
    }

    sElapsedSeconds %= COMPLETE_CYCLES_SECONDS;

    for (i = 0; i < CLOCK_CYCLE_COUNT; i++) {
        const struct ClockCycle *cycle = &sClockCycles[i];

        if (sElapsedSeconds < cycle->CycleSeconds) {
            sGameSecond = (cycle->CycleStartSecond + sElapsedSeconds * cycle->CycleInGameSeconds / cycle->CycleSeconds) % SECONDS_PER_DAY;
            break;
        }

        sElapsedSeconds -= cycle->CycleSeconds;
    }

    if (SaveBlock2_get() != NULL) {
        UpdateApricornTrees(sGameSecond);
    }

    return sGameSecond;
}