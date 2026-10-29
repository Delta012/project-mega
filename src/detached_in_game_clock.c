#include "config.h"
#include "types.h"

#include "rtc.h"
#include "save.h"
#include "system.h"

// ============================================================================================================
// DETACHED IN-GAME CLOCK
//
// In the vanilla game the time of day follows the real time of your DS.
// Here the game has its own clock instead: it only moves while you are playing (like the newest games).
//
// - The hour, minute and second are the game's own and follow your play time.
// - The date, the day of the week and the season still come from the real calendar.
// - How long morning / day / evening / night last depends on the season. This can be changed by looking at ~\include\config.h.
// - The hours at which morning / day / evening / night start depend on the season too (see "HOURS OF EACH SEASON").
//   Music, map colors, wild Pokemon, battle backgrounds, evolutions... all follow them.
// - Apricorn trees, Berry Pots, Pokégear and every other thing that checked the time has been converted to use the new system too.
// ============================================================================================================

// ============================================================================================================
// SAVE VARIABLES
// ============================================================================================================

// These are free script variables of the save file. No vanilla script uses them.
#define GAME_CLOCK_ELAPSED_SECONDS_VAR 0x40E4   // Seconds played in the current loop (a loop = one game day, from 00:00 to 24:00)

#define GAME_CLOCK_VARS_BASE           0x4059
#define GAME_CLOCK_APRICORN_DAYS_VAR   GAME_CLOCK_VARS_BASE                 // Game days passed since the apricorn trees grew back
#define GAME_CLOCK_BERRY_CLOCK_VAR     (GAME_CLOCK_VARS_BASE + 1)           // Seconds played since the save was created (uses 2 variables)
#define GAME_CLOCK_BERRY_STAMP_VAR     (GAME_CLOCK_VARS_BASE + 3)           // For each Berry Pot: the berry clock when it was last updated (2 variables per pot)
#define GAME_CLOCK_SEASON_VAR          (GAME_CLOCK_BERRY_STAMP_VAR + MAX_BERRY_POT * 2) // The season of the current loop (1 = spring ... 4 = winter, 0 = not set)
#define GAME_CLOCK_VARS_END            (GAME_CLOCK_SEASON_VAR + 1)

// ============================================================================================================
// HOURS OF EACH SEASON
//
// The game hours (0 - 23) at which each part of the day starts and ends, for each season.
// They are in ~\include\config.h, next to the minutes each part lasts in real time. For every season (SPRING, SUMMER, AUTUMN, WINTER):
//
//   GAME_CLOCK_<SEASON>_MORNING_START_HOUR    GAME_CLOCK_<SEASON>_MORNING_END_HOUR
//   GAME_CLOCK_<SEASON>_DAY_START_HOUR        GAME_CLOCK_<SEASON>_DAY_END_HOUR
//   GAME_CLOCK_<SEASON>_EVENING_START_HOUR    GAME_CLOCK_<SEASON>_EVENING_END_HOUR
//   GAME_CLOCK_<SEASON>_NIGHT_START_HOUR      GAME_CLOCK_<SEASON>_NIGHT_END_HOUR
//
// Each part ends when the next one starts (the night ends when the morning starts), so the build fails if they do not match.
// ============================================================================================================

// NUMBERS USED BY THE CLOCK
#define SECONDS_PER_DAY 86400

// The DS runs at about 59.83 frames per second. Using 60 makes the game clock 0.3% slower.
#define FRAMES_PER_SECOND 60

// A berry that needs 1 "hour" in the vanilla game grows with 60 vanilla minutes
#define BERRY_MINUTES_PER_HOUR 60

// How many real seconds a full loop (morning + day + evening + night) lasts in a season
#define SEASON_LOOP_SECONDS(season) ((GAME_CLOCK_##season##_MORNING_MINUTES + GAME_CLOCK_##season##_DAY_MINUTES \
                                    + GAME_CLOCK_##season##_EVENING_MINUTES + GAME_CLOCK_##season##_NIGHT_MINUTES) * 60)

// How many real seconds each part of the day lasts in a season
#define SEASON_CYCLE_SECONDS(season) { GAME_CLOCK_##season##_MORNING_MINUTES * 60, GAME_CLOCK_##season##_DAY_MINUTES * 60, \
                                       GAME_CLOCK_##season##_EVENING_MINUTES * 60, GAME_CLOCK_##season##_NIGHT_MINUTES * 60 }

enum DailyClockCycles {
    CLOCK_CYCLE_MORNING,
    CLOCK_CYCLE_DAY,
    CLOCK_CYCLE_EVENING,
    CLOCK_CYCLE_NIGHT,
    CLOCK_CYCLE_COUNT,
};

enum ClockSeasons {
    CLOCK_SEASON_SPRING,
    CLOCK_SEASON_SUMMER,
    CLOCK_SEASON_AUTUMN,
    CLOCK_SEASON_WINTER,
    CLOCK_SEASON_COUNT,
};

// ============================================================================================================
// SAFETY CHECKS
// ============================================================================================================

// If you write a wrong number in your ~\include\config.h, the build fails the compilation.
// Without these checks, the game would just break while you play.
#define CLOCK_STATIC_ASSERT(name, condition) typedef char name[(condition) ? 1 : -1]

// Every part of the day must last at least 1 minute
#define SEASON_CYCLES_VALID(season) (GAME_CLOCK_##season##_MORNING_MINUTES > 0 && GAME_CLOCK_##season##_DAY_MINUTES > 0 \
                                  && GAME_CLOCK_##season##_EVENING_MINUTES > 0 && GAME_CLOCK_##season##_NIGHT_MINUTES > 0)

CLOCK_STATIC_ASSERT(SpringCyclesNotEmpty, SEASON_CYCLES_VALID(SPRING));
CLOCK_STATIC_ASSERT(SummerCyclesNotEmpty, SEASON_CYCLES_VALID(SUMMER));
CLOCK_STATIC_ASSERT(AutumnCyclesNotEmpty, SEASON_CYCLES_VALID(AUTUMN));
CLOCK_STATIC_ASSERT(WinterCyclesNotEmpty, SEASON_CYCLES_VALID(WINTER));

// A full loop must fit in a save variable (max 65535 seconds, about 18 hours)
CLOCK_STATIC_ASSERT(SpringLoopFitsInVar, SEASON_LOOP_SECONDS(SPRING) <= 0xFFFF);
CLOCK_STATIC_ASSERT(SummerLoopFitsInVar, SEASON_LOOP_SECONDS(SUMMER) <= 0xFFFF);
CLOCK_STATIC_ASSERT(AutumnLoopFitsInVar, SEASON_LOOP_SECONDS(AUTUMN) <= 0xFFFF);
CLOCK_STATIC_ASSERT(WinterLoopFitsInVar, SEASON_LOOP_SECONDS(WINTER) <= 0xFFFF);

// The parts of the day must be in order inside the same day. The morning starts before noon and the night in the afternoon or later.
// (The night is the only part that goes past midnight.) Every END hour must be the START hour of the next part.
#define SEASON_HOURS_VALID(season) (GAME_CLOCK_##season##_MORNING_START_HOUR >= 0 && GAME_CLOCK_##season##_MORNING_START_HOUR <= 12 \
                                 && GAME_CLOCK_##season##_MORNING_START_HOUR < GAME_CLOCK_##season##_DAY_START_HOUR \
                                 && GAME_CLOCK_##season##_DAY_START_HOUR < GAME_CLOCK_##season##_EVENING_START_HOUR \
                                 && GAME_CLOCK_##season##_EVENING_START_HOUR < GAME_CLOCK_##season##_NIGHT_START_HOUR \
                                 && GAME_CLOCK_##season##_NIGHT_START_HOUR >= 12 && GAME_CLOCK_##season##_NIGHT_START_HOUR <= 23 \
                                 && GAME_CLOCK_##season##_MORNING_END_HOUR == GAME_CLOCK_##season##_DAY_START_HOUR \
                                 && GAME_CLOCK_##season##_DAY_END_HOUR == GAME_CLOCK_##season##_EVENING_START_HOUR \
                                 && GAME_CLOCK_##season##_EVENING_END_HOUR == GAME_CLOCK_##season##_NIGHT_START_HOUR \
                                 && GAME_CLOCK_##season##_NIGHT_END_HOUR == GAME_CLOCK_##season##_MORNING_START_HOUR)

CLOCK_STATIC_ASSERT(SpringHoursValid, SEASON_HOURS_VALID(SPRING));
CLOCK_STATIC_ASSERT(SummerHoursValid, SEASON_HOURS_VALID(SUMMER));
CLOCK_STATIC_ASSERT(AutumnHoursValid, SEASON_HOURS_VALID(AUTUMN));
CLOCK_STATIC_ASSERT(WinterHoursValid, SEASON_HOURS_VALID(WINTER));

CLOCK_STATIC_ASSERT(ClockNewGameCycleValid, GAME_CLOCK_NEW_GAME_CYCLE >= 0 && GAME_CLOCK_NEW_GAME_CYCLE < CLOCK_CYCLE_COUNT);
CLOCK_STATIC_ASSERT(ClockVarsInSaveRange, GAME_CLOCK_VARS_BASE >= 0x4020 && GAME_CLOCK_VARS_END - 1 <= 0x416F);
CLOCK_STATIC_ASSERT(ClockVarsDistinct, GAME_CLOCK_ELAPSED_SECONDS_VAR < GAME_CLOCK_VARS_BASE || GAME_CLOCK_ELAPSED_SECONDS_VAR >= GAME_CLOCK_VARS_END);
CLOCK_STATIC_ASSERT(ApricornRegrowDaysValid, APRICORN_REGROW_DAYS >= 1 && APRICORN_REGROW_DAYS <= 0xFFFF);
CLOCK_STATIC_ASSERT(BerryRegrowDaysValid, BERRY_REGROW_DAYS >= 1);

// ============================================================================================================
// TABLES
// ============================================================================================================

// The game hour (0 - 23) at which each part of the day starts, for each season (see "HOURS OF EACH SEASON").
#define SEASON_START_HOURS(season) { GAME_CLOCK_##season##_MORNING_START_HOUR, GAME_CLOCK_##season##_DAY_START_HOUR, \
                                     GAME_CLOCK_##season##_EVENING_START_HOUR, GAME_CLOCK_##season##_NIGHT_START_HOUR }

static const u8 sSeasonStartHours[CLOCK_SEASON_COUNT][CLOCK_CYCLE_COUNT] = {
    [CLOCK_SEASON_SPRING] = SEASON_START_HOURS(SPRING),
    [CLOCK_SEASON_SUMMER] = SEASON_START_HOURS(SUMMER),
    [CLOCK_SEASON_AUTUMN] = SEASON_START_HOURS(AUTUMN),
    [CLOCK_SEASON_WINTER] = SEASON_START_HOURS(WINTER),
};

// The game seconds of the day at which each part starts in the vanilla game (04:00, 10:00, 17:00, 20:00).
// The game's own code (music, map colors, wild Pokemon, battle backgrounds...) only knows these hours.
// The clock gives it a time where the parts of the current season are moved onto these hours (see GetEngineSecondOfDay).
static const u32 sVanillaCycleStartSeconds[CLOCK_CYCLE_COUNT] = {
    [CLOCK_CYCLE_MORNING] =  4 * 3600,
    [CLOCK_CYCLE_DAY]     = 10 * 3600,
    [CLOCK_CYCLE_EVENING] = 17 * 3600,
    [CLOCK_CYCLE_NIGHT]   = 20 * 3600,
};

// How many real seconds each part of the day lasts, for each season.
static const u16 sSeasonCycleSeconds[CLOCK_SEASON_COUNT][CLOCK_CYCLE_COUNT] = {
    [CLOCK_SEASON_SPRING] = SEASON_CYCLE_SECONDS(SPRING),
    [CLOCK_SEASON_SUMMER] = SEASON_CYCLE_SECONDS(SUMMER),
    [CLOCK_SEASON_AUTUMN] = SEASON_CYCLE_SECONDS(AUTUMN),
    [CLOCK_SEASON_WINTER] = SEASON_CYCLE_SECONDS(WINTER),
};

// How many real seconds a full loop lasts, for each season.
static const u16 sSeasonLoopSeconds[CLOCK_SEASON_COUNT] = {
    [CLOCK_SEASON_SPRING] = SEASON_LOOP_SECONDS(SPRING),
    [CLOCK_SEASON_SUMMER] = SEASON_LOOP_SECONDS(SUMMER),
    [CLOCK_SEASON_AUTUMN] = SEASON_LOOP_SECONDS(AUTUMN),
    [CLOCK_SEASON_WINTER] = SEASON_LOOP_SECONDS(WINTER),
};

// The season of each month (1 = January ... 12 = December), for the northern hemisphere.
static const u8 sSeasonOfMonth[13] = {
    CLOCK_SEASON_WINTER,                                                        // 0: not a real month
    CLOCK_SEASON_WINTER, CLOCK_SEASON_WINTER,                                   // Jan, Feb
    CLOCK_SEASON_SPRING, CLOCK_SEASON_SPRING, CLOCK_SEASON_SPRING,              // Mar, Apr, May
    CLOCK_SEASON_SUMMER, CLOCK_SEASON_SUMMER, CLOCK_SEASON_SUMMER,              // Jun, Jul, Aug
    CLOCK_SEASON_AUTUMN, CLOCK_SEASON_AUTUMN, CLOCK_SEASON_AUTUMN,              // Sep, Oct, Nov
    CLOCK_SEASON_WINTER,                                                        // Dec
};

// ============================================================================================================
// THE CLOCK
// ============================================================================================================

static u32 sLastVblank;                         // The frame counter the last time the clock was read
static u32 sFrameRemainder;                     // Frames that are not a full second yet
static u32 sLastGameSecond = SECONDS_PER_DAY;   // The game second seen last time. SECONDS_PER_DAY means "none yet"
static u32 sLoopSeason;                         // Season of the current loop (set by GetCycleElapsedSeconds)
static BOOL sTickReady;                         // FALSE until the first read, so the first read adds no time

// A save variable holds 16 bits. These two functions join 2 variables to make a 32 bit number.
static u32 GetVar32(u16 var)
{
    return GetScriptVar(var) | ((u32)GetScriptVar(var + 1) << 16);
}

static void SetVar32(u16 var, u32 value)
{
    SetScriptVar(var, value & 0xFFFF);
    SetScriptVar(var + 1, value >> 16);
}

// The season of today, from the real calendar.
static u32 GetRealSeason(void)
{
    struct RTCDate date;

    GF_RTC_CopyDate(&date);

    if (date.month < 1 || date.month > 12) {
        return CLOCK_SEASON_SPRING;
    }

    return sSeasonOfMonth[date.month];
}

// The loop is one game day. It is split in 5 pieces, because the night goes past midnight:
//   [0] the end of the night (00:00 - morning)   [1] morning   [2] day   [3] evening   [4] the start of the night (night - 24:00)
// Each piece has its own game hours and its own real seconds. The hours depend on the season.
#define CLOCK_SEGMENT_COUNT 5

struct ClockSegment {
    u32 gameStart;      // The game second of the day when this piece starts
    u32 gameSeconds;    // How many game seconds this piece lasts
    u32 realSeconds;    // How many real seconds this piece lasts
};

static void GetSeasonSegments(u32 season, struct ClockSegment segments[CLOCK_SEGMENT_COUNT])
{
    const u8 *hours = sSeasonStartHours[season];
    const u16 *real = sSeasonCycleSeconds[season];
    u32 morningStart = hours[CLOCK_CYCLE_MORNING] * 3600;
    u32 dayStart = hours[CLOCK_CYCLE_DAY] * 3600;
    u32 eveningStart = hours[CLOCK_CYCLE_EVENING] * 3600;
    u32 nightStart = hours[CLOCK_CYCLE_NIGHT] * 3600;

    // The night is one single part of the config, but it is on both sides of midnight.
    // Its real seconds are shared between the two sides by their game hours, so the night does not speed up or slow down at midnight.
    u32 nightGameSeconds = (SECONDS_PER_DAY - nightStart) + morningStart;
    u32 nightEndRealSeconds = real[CLOCK_CYCLE_NIGHT] * morningStart / nightGameSeconds;

    segments[0].gameStart = 0;
    segments[0].gameSeconds = morningStart;
    segments[0].realSeconds = nightEndRealSeconds;

    segments[1].gameStart = morningStart;
    segments[1].gameSeconds = dayStart - morningStart;
    segments[1].realSeconds = real[CLOCK_CYCLE_MORNING];

    segments[2].gameStart = dayStart;
    segments[2].gameSeconds = eveningStart - dayStart;
    segments[2].realSeconds = real[CLOCK_CYCLE_DAY];

    segments[3].gameStart = eveningStart;
    segments[3].gameSeconds = nightStart - eveningStart;
    segments[3].realSeconds = real[CLOCK_CYCLE_EVENING];

    segments[4].gameStart = nightStart;
    segments[4].gameSeconds = SECONDS_PER_DAY - nightStart;
    segments[4].realSeconds = real[CLOCK_CYCLE_NIGHT] - nightEndRealSeconds;
}

// The seconds of a loop that have passed when a part of the day starts. Used to start a new game at the right part.
static u32 GetCycleStartElapsedSeconds(u32 season, int cycle)
{
    struct ClockSegment segments[CLOCK_SEGMENT_COUNT];
    u32 elapsedSeconds = 0;
    int i;

    GetSeasonSegments(season, segments);

    // The morning starts after piece [0], the day after [0] - [1], the evening after [0] - [2], the night after [0] - [3]
    for (i = 0; i <= cycle; i++) {
        elapsedSeconds += segments[i].realSeconds;
    }

    return elapsedSeconds;
}

// The season the current loop uses. It is saved, so a loop keeps the same season until it ends.
static u32 GetLoopSeason(void)
{
    u32 stored = GetScriptVar(GAME_CLOCK_SEASON_VAR);

    // Nothing saved yet (new game): use today's season and start the loop where the new game starts
    if (stored == 0 || stored > CLOCK_SEASON_COUNT) {
        stored = GetRealSeason() + 1;
        SetScriptVar(GAME_CLOCK_SEASON_VAR, stored);
        SetScriptVar(GAME_CLOCK_ELAPSED_SECONDS_VAR, GetCycleStartElapsedSeconds(stored - 1, GAME_CLOCK_NEW_GAME_CYCLE));
    }

    return stored - 1;
}

// How many seconds of the current loop you have played. This is the core of the clock feature.
// It adds the time that passed since the last call and saves it.
// A loop is always one game day, from 00:00 to 24:00. When it ends, the next one starts at 00:00 with the season of today,
// so the hour never jumps when the season changes.
static u32 GetCycleElapsedSeconds(void)
{
    u32 now = gSystem.vblankCounter;
    u32 loopSeconds;
    u32 elapsedSeconds;

    // No save loaded yet (title screen): the clock does not run.
    if (SaveBlock2_get() == NULL) {
        return 0;
    }

    sLoopSeason = GetLoopSeason();
    loopSeconds = sSeasonLoopSeconds[sLoopSeason];
    elapsedSeconds = GetScriptVar(GAME_CLOCK_ELAPSED_SECONDS_VAR) % loopSeconds;

    if (sTickReady && now > sLastVblank) {
        u32 seconds;

        // Turn the frames that passed into seconds. Frames left over wait for the next call.
        sFrameRemainder += now - sLastVblank;
        seconds = sFrameRemainder / FRAMES_PER_SECOND;
        sFrameRemainder %= FRAMES_PER_SECOND;

        if (seconds != 0) {
            elapsedSeconds += seconds;

            // The loop is over (it is midnight): start the next one, with the season of today
            while (elapsedSeconds >= loopSeconds) {
                elapsedSeconds -= loopSeconds;
                sLoopSeason = GetRealSeason();
                loopSeconds = sSeasonLoopSeconds[sLoopSeason];
            }

            SetScriptVar(GAME_CLOCK_ELAPSED_SECONDS_VAR, elapsedSeconds);
            SetScriptVar(GAME_CLOCK_SEASON_VAR, sLoopSeason + 1);
            SetVar32(GAME_CLOCK_BERRY_CLOCK_VAR, GetVar32(GAME_CLOCK_BERRY_CLOCK_VAR) + seconds);
        }
    }

    sLastVblank = now;
    sTickReady = TRUE;

    return elapsedSeconds;
}

// ============================================================================================================
// APRICORN TREES
// ============================================================================================================

// Same as the vanilla function sub_0202AE74: every tree without apricorns gets them back.
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

// Counts the game days. Every "APRICORN_REGROW_DAYS" in-game days, the trees grow back.
static void UpdateApricornTrees(u32 gameSecond)
{
    // The second went backwards, so the game clock passed midnight: one in-game day is over.
    if (sLastGameSecond != SECONDS_PER_DAY && gameSecond < sLastGameSecond) {
        u32 days = GetScriptVar(GAME_CLOCK_APRICORN_DAYS_VAR) + 1;

        if (days >= APRICORN_REGROW_DAYS) {
            RegrowApricornTrees();
            days = 0;
        }

        SetScriptVar(GAME_CLOCK_APRICORN_DAYS_VAR, days);
    }

    sLastGameSecond = gameSecond;
}

// ============================================================================================================
// GAME TIME
// ============================================================================================================

// Turns the seconds of the loop into the game second of the day, using the hours of the season of the loop.
static u32 RealToGameSecond(u32 season, u32 elapsedSeconds)
{
    struct ClockSegment segments[CLOCK_SEGMENT_COUNT];
    int i;

    GetSeasonSegments(season, segments);

    // Go through the pieces one by one, until we find the one that contains the played time.
    for (i = 0; i < CLOCK_SEGMENT_COUNT; i++) {
        const struct ClockSegment *segment = &segments[i];

        if (elapsedSeconds < segment->realSeconds) {
            // Found it. Place the time inside the game hours of this piece.
            return segment->gameStart + elapsedSeconds * segment->gameSeconds / segment->realSeconds;
        }

        elapsedSeconds -= segment->realSeconds;
    }

    return 0; // Out of the loop. It can't happen, because the elapsed seconds are always smaller than the loop.
}

// The game second of the day (0 - 86399). 0 is midnight, 43200 is noon.
// This is the time the player sees: the parts of the day start at the hours of the season.
static u32 GetGameSecondOfDay(void)
{
    u32 elapsedSeconds = GetCycleElapsedSeconds();
    u32 gameSecond = RealToGameSecond(sLoopSeason, elapsedSeconds);

    if (SaveBlock2_get() != NULL) {
        UpdateApricornTrees(gameSecond);
    }

    return gameSecond;
}

// The second of the day that the game's own code (music, map colors, wild Pokemon, battle backgrounds, evolutions...) must see.
// That code has the vanilla hours inside (the night starts at 20:00, the morning at 04:00...), and it can't know about the seasons.
// So the parts of the current season are stretched or squeezed onto the vanilla hours:
// when the winter night starts (19:00 for the player), the game's code sees 20:00. Midnight stays midnight.
static u32 GetEngineSecondOfDay(void)
{
    u32 gameSecond = GetGameSecondOfDay();
    const u8 *hours = sSeasonStartHours[sLoopSeason];
    u32 from[CLOCK_CYCLE_COUNT + 2];
    u32 to[CLOCK_CYCLE_COUNT + 2];
    int i;

    from[0] = 0;
    to[0] = 0;

    for (i = 0; i < CLOCK_CYCLE_COUNT; i++) {
        from[i + 1] = hours[i] * 3600;
        to[i + 1] = sVanillaCycleStartSeconds[i];
    }

    from[CLOCK_CYCLE_COUNT + 1] = SECONDS_PER_DAY;
    to[CLOCK_CYCLE_COUNT + 1] = SECONDS_PER_DAY;

    // Find the piece of the day that contains the time and move it onto the vanilla hours
    for (i = 0; i <= CLOCK_CYCLE_COUNT; i++) {
        if (gameSecond < from[i + 1]) {
            return to[i] + (gameSecond - from[i]) * (to[i + 1] - to[i]) / (from[i + 1] - from[i]);
        }
    }

    return gameSecond;
}

// The time the player sees (hour, minute, second). Used by the Pokégear and the radio.
static void GetGameTime(struct RTCTime *time)
{
    u32 gameSecond = GetGameSecondOfDay();

    time->hour = gameSecond / 3600;
    time->minute = (gameSecond % 3600) / 60;
    time->second = gameSecond % 60;
}

// ============================================================================================================
// FUNCTIONS THAT REPLACE VANILLA ONES (see ~\hooks)
// ============================================================================================================

// Gives the hour, minute and second. Used by wild encounters, evolutions, music and Gracidea.
// The game's code only knows the vanilla hours, so this is the time of the season moved onto them (see GetEngineSecondOfDay).
// The time the player sees is in GetGameTime.
void LONG_CALL GF_RTC_CopyTime_Detached(struct RTCTime *time)
{
    u32 engineSecond = GetEngineSecondOfDay();

    time->hour = engineSecond / 3600;
    time->minute = (engineSecond % 3600) / 60;
    time->second = engineSecond % 60;
}

// Gives the second of the day. Used by the colors (tint) of the map. Same time as GF_RTC_CopyTime_Detached.
s32 LONG_CALL GF_RTC_TimeToSec_Detached(void)
{
    return (s32)GetEngineSecondOfDay();
}

// Tells if it is night right now, using the hours of the season of the loop.
// Use it when the code must not depend on the vanilla hours (for example Shaymin's Sky Forme).
BOOL LONG_CALL GameClock_IsNight_Detached(void)
{
    u32 gameSecond = GetGameSecondOfDay();
    const u8 *hours = sSeasonStartHours[sLoopSeason];

    return gameSecond >= (u32)hours[CLOCK_CYCLE_NIGHT] * 3600 || gameSecond < (u32)hours[CLOCK_CYCLE_MORNING] * 3600;
}

// Gives morning / day / evening / night. Used by battle backgrounds, the GetTimeOfDay script command and NPC trades.
// The vanilla function is fine: it already asks GF_RTC_CopyTime, which is the game clock now (with the hours of the season).
TIMEOFDAY LONG_CALL Field_GetTimeOfDay_Detached(void *fieldSystem)
{
    (void)fieldSystem;
    return GF_RTC_GetTimeOfDay();
}

// Gives the time of day for wild Pokemon. Used by NPC rematches.
TimeOfDayWildParam LONG_CALL Field_GetTimeOfDayWildParam_Detached(void *fieldSystem)
{
    (void)fieldSystem;
    return GF_RTC_GetTimeOfDayWildParam();
}

// Gives only the hour (the one the game's code sees, like GF_RTC_CopyTime_Detached).
u32 LONG_CALL Field_GetHour_Detached(void *fieldSystem)
{
    (void)fieldSystem;
    return GetEngineSecondOfDay() / 3600;
}

// Replaces the vanilla daily apricorn regrow (sub_020556B8). It does nothing on purpose:
// the trees now grow back by game days (see UpdateApricornTrees), not by real days.
void LONG_CALL Field_RegrowApricornTrees_Detached(void *fieldSystem)
{
    (void)fieldSystem;
}


// ============================================================================================================
// BERRY POTS
//
// Berry pots grow based on the in-game time. A berry that needs "x" vanilla hours takes"
// BERRY_REGROW_DAYS in-game days * (1 + x / 100) of playtime for each growth stage.
// Example with 4 days: X = 36 -> 4 days * 1.36.
// A game day lasts as long as a day of the season of the current loop.
// ============================================================================================================

// Vanilla functions in the overlays. Thumb functions need "| 1" at the end of the address.
#define ov16_02201760 ((void (*)(BerryPot *, const void *, s32))(0x02201760u | 1u))  // Grows the given pots by some vanilla minutes
#define ov17_0220387C ((void (*)(void *))(0x0220387Cu | 1u))                         // Redraws the berry pots app
#define ov17_02203928 ((void (*)(void *))(0x02203928u | 1u))                         // Redraws the berry pots app

// Only the first fields of BerryPotsAppData (the bytes 0x00 - 0x1F), the rest is not needed
struct BerryPotsAppDataHead {
    u32 unused[5];
    BerryPot *berryPots;
    void *unusedDatetime;
    void *berryGrowthProperties;
};

// What the game knows about each berry (3 bytes). The list starts at berry number 1.
struct BerryGrowthProperties {
    u8 growthHours;
    u8 drainRate;
    u8 unk2;
};

// The vanilla function grows 4 pots at once. We need to grow only one, so we give it
// a list with this pot and 3 empty pots (berryId 0 = empty, the function skips them).
static void GrowSingleBerryPot(BerryPot *pot, const struct BerryGrowthProperties *props, u32 minutes)
{
    BerryPot single[MAX_BERRY_POT];
    int i;

    single[0] = *pot;
    for (i = 1; i < MAX_BERRY_POT; i++) {
        single[i].berryId = 0;
    }

    ov16_02201760(single, props, minutes);
    *pot = single[0];
}

// Replaces the vanilla berry growth update (ov17_022037C8)
void LONG_CALL BerryPots_UpdateGrowth_Detached(struct BerryPotsAppDataHead *data, BOOL a1)
{
    const struct BerryGrowthProperties *props = (const struct BerryGrowthProperties *)data->berryGrowthProperties;
    u32 berryClock;
    BOOL anyGrown = FALSE;
    int i;

    // Reading the clock adds the time played since the last read, and sets the season of the loop
    (void)GetCycleElapsedSeconds();
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

        // Empty pot: nothing grows. Its time starts counting when you plant a berry.
        if (pot->berryId == 0) {
            SetVar32(stampVar, berryClock);
            continue;
        }

        hours = props[pot->berryId - 1].growthHours;
        rate = hours * BERRY_MINUTES_PER_HOUR;
        if (rate == 0) {
            continue;
        }

        // Seconds of play that one growth stage needs
        secondsPerStage = BERRY_REGROW_DAYS * sSeasonLoopSeconds[sLoopSeason] * (100 + hours) / 100;

        // Seconds this pot has been growing. The limit keeps the multiplication below inside 32 bits.
        stamp = GetVar32(stampVar);
        pending = berryClock - stamp;
        if (pending > (0xFFFFFFFF - rate) / rate) {
            pending = (0xFFFFFFFF - rate) / rate;
        }

        // Turn those seconds into vanilla minutes of growth
        minutes = pending * rate / secondsPerStage;
        if (minutes == 0) {
            continue;
        }

        // Spend only the seconds those minutes are worth. The leftover seconds wait for the next update.
        SetVar32(stampVar, stamp + (minutes * secondsPerStage + rate - 1) / rate);

        GrowSingleBerryPot(pot, props, minutes);
        anyGrown = TRUE;
    }

    // Something grew: redraw the app
    if (anyGrown) {
        ov17_0220387C(data);
        if (a1) {
            ov17_02203928(data);
        }
    }
}

// ============================================================================================================
// POKÉGEAR CLOCK
// The Pokégear now showsshow the in-game time (the one of the season). The weekday is still the real one.
// ============================================================================================================

// Address of ManagedSprite_SetAnimationFrame (an arm9 function).
// The last bit is 1 because it is a Thumb function.
#define ManagedSprite_SetAnimationFrame_ADDRESS 0x0200DCC1u
#define ManagedSprite_SetAnimationFrame ((void (*)(void *, u16))ManagedSprite_SetAnimationFrame_ADDRESS)

CLOCK_STATIC_ASSERT(PokegearSpriteAddressSet, ManagedSprite_SetAnimationFrame_ADDRESS != 0);

// Where things are inside PokegearAppData
#define POKEGEAR_NEED_CLOCK_UPDATE_OFFSET 0x07  // u8: 1 = the clock must be redrawn
#define POKEGEAR_TIME_OFFSET              0x80  // The time drawn last time (RTCTime)
#define POKEGEAR_UI_SPRITES_OFFSET        0x98  // List of 11 sprites. Number 4 = weekday, numbers 5 to 8 = the 4 digits (HH:MM)

// Replaces PokegearApp_UpdateClockSprites. Returns TRUE if the clock was redrawn.
BOOL LONG_CALL PokegearApp_UpdateClockSprites_Detached(void *pokegearApp, BOOL force)
{
    u8 *app = (u8 *)pokegearApp;
    struct RTCTime *lastTime = (struct RTCTime *)(app + POKEGEAR_TIME_OFFSET);
    void **uiSprites = (void **)(app + POKEGEAR_UI_SPRITES_OFFSET);
    struct RTCDate date;
    struct RTCTime time;
    u8 digits[4];
    u8 i;

    GF_RTC_CopyDate(&date);
    GetGameTime(&time);

    // Nothing to do if the second is the same as last time (unless we are forced to redraw)
    if (!force && lastTime->second == time.second) {
        return FALSE;
    }

    // Split HH:MM into 4 digits
    digits[0] = time.hour / 10;
    digits[1] = time.hour % 10;
    digits[2] = time.minute / 10;
    digits[3] = time.minute % 10;

    // Each digit sprite has one frame per number, so the digit is the frame
    for (i = 0; i < 4; i++) {
        ManagedSprite_SetAnimationFrame(uiSprites[i + 5], digits[i]);
    }

    ManagedSprite_SetAnimationFrame(uiSprites[4], date.week);
    *lastTime = time;
    app[POKEGEAR_NEED_CLOCK_UPDATE_OFFSET] = 0;
    return TRUE;
}

// ============================================================================================================
// RADIO
//
// Every radio station that changes during the day has a list, and every line of it is a rule:
//
//       { DAYS,      HOUR,  SHOW }
//       { EVERYDAY,  8,     DRAMA }     <- "every day, from 08:00, play the Radio Drama"
//
// The radio checks the rules from top to bottom. The last rule that fits the day and the hour wins.
// That is why the first rule of a list must be { EVERYDAY, 0, ... }: it is the show for the start of the day,
// and the next rules replace it later in the day.
//
// DAYS:  MON  TUE  WED  THU  FRI  SAT  SUN
//        To pick more days, put a | between them: SAT | SUN
//        Shortcuts are also available: WEEKDAYS = Monday to Friday, WEEKEND = Saturday and Sunday, EVERYDAY = Monday to Sunday
// HOUR:  A number from 0 to 23.
// SHOW:  TRAINERS, TOWN, SEARCH, DRAMA, BUENA.
//
// EXAMPLE: Radio Drama from 08:00, but on Sunday Buena's Password plays from 08:00 instead:
//       { EVERYDAY, 0, SEARCH },
//       { EVERYDAY, 8, DRAMA },
//       { SUN,      8, BUENA },     <- comes after the Drama rule, so on Sunday it wins
// ============================================================================================================

// The number of every show. The game uses these numbers, do not change them.
#define RADIO_SHOW_POKEMON_MUSIC          0
#define RADIO_SHOW_POKEMON_TALK           1
#define RADIO_SHOW_POKEMON_SEARCH_PARTY   2
#define RADIO_SHOW_SERIAL_RADIO_DRAMA     3
#define RADIO_SHOW_BUENAS_PASSWORD        4
#define RADIO_SHOW_TRAINER_PROFILES       5
#define RADIO_SHOW_THAT_TOWN_THESE_PEOPLE 6
#define RADIO_SHOW_POKE_FLUTE             7
#define RADIO_SHOW_UNOWN                  8
#define RADIO_SHOW_TEAM_ROCKET            9
#define RADIO_SHOW_MAHOGANY_SIGNAL        10
#define RADIO_SHOW_COMMERCIALS            11

// Short names of the shows you can use in the lists
#define TRAINERS RADIO_SHOW_TRAINER_PROFILES
#define TOWN     RADIO_SHOW_THAT_TOWN_THESE_PEOPLE
#define SEARCH   RADIO_SHOW_POKEMON_SEARCH_PARTY
#define DRAMA    RADIO_SHOW_SERIAL_RADIO_DRAMA
#define BUENA    RADIO_SHOW_BUENAS_PASSWORD

// The days. The game counts them from Sunday (0) to Saturday (6). Every day is one bit, so they can be joined with |
#define SUN (1 << 0)
#define MON (1 << 1)
#define TUE (1 << 2)
#define WED (1 << 3)
#define THU (1 << 4)
#define FRI (1 << 5)
#define SAT (1 << 6)
#define WEEKDAYS (MON | TUE | WED | THU | FRI)
#define WEEKEND  (SAT | SUN)
#define EVERYDAY (WEEKDAYS | WEEKEND)

struct RadioSlot {
    u8 days;        // The days this rule is for
    u8 startHour;   // The game hour (0 - 23) when the show starts
    u8 show;        // The show to play
};

// Schedule

// Dial position 3. It plays Trainer Profiles or That Town, These People.
static const struct RadioSlot sRadioStation2[] = {
    { EVERYDAY,  0, TRAINERS },   // 00:00 - 11:59
    { EVERYDAY, 12, TOWN },       // 12:00 - 23:59
};

// Dial position 4. It plays Search Party, Radio Drama or Buena's Password.
static const struct RadioSlot sRadioStation3[] = {
    { EVERYDAY,  0, SEARCH },     // 00:00 - 07:59
    { EVERYDAY,  8, DRAMA },      // 08:00 - 15:59
    { EVERYDAY, 16, BUENA },      // 16:00 - 23:59
};

// End of Schedule

// Looks at the rules of a list from top to bottom and gives the show of the last rule that fits this day and hour
static u8 GetScheduledShow(const struct RadioSlot *list, u32 count, u32 week, u32 hour)
{
    u8 show = list[0].show;
    u32 i;

    for (i = 0; i < count; i++) {
        if ((list[i].days & (1 << week)) && list[i].startHour <= hour) {
            show = list[i].show;
        }
    }

    return show;
}

#define LIST_SIZE(list) (sizeof(list) / sizeof((list)[0]))

// Where things are stored inside the radio data (the game's RadioShow). These are fixed, do not change them.
#define RADIO_DATE_OFFSET       0x28    // RTCDate: the show reads the weekday from here
#define RADIO_TIME_OFFSET       0x38    // RTCTime
#define RADIO_FLAGS_OFFSET      0x66    // u8 with several 1 bit flags
#define RADIO_FLAG_COMMERCIALS  0x10    // The flag "play the commercials next"

// Replaces the vanilla RadioShow_TranslateStationID.
// The radio asks: "which show should play on dial position X (0 - 7)?" This function answers.
u8 LONG_CALL RadioShow_TranslateStationID_Detached(void *radioShow, int station)
{
    u8 *radio = (u8 *)radioShow;
    struct RTCDate *date = (struct RTCDate *)(radio + RADIO_DATE_OFFSET);
    struct RTCTime *time = (struct RTCTime *)(radio + RADIO_TIME_OFFSET);

    if (station >= 8) {
        station = 0;
    }

    // The last show asked for the commercials: play them now, one time
    if (radio[RADIO_FLAGS_OFFSET] & RADIO_FLAG_COMMERCIALS) {
        radio[RADIO_FLAGS_OFFSET] &= ~RADIO_FLAG_COMMERCIALS;
        return RADIO_SHOW_COMMERCIALS;
    }

    // Save the date and time inside the radio data, because the shows read them later.
    // The date is the real one. The time is the game clock (the one the player sees).
    GF_RTC_CopyDate(date);
    GetGameTime(time);

    switch (station) {
    case 0:
        return RADIO_SHOW_POKEMON_MUSIC;
    case 1:
        return RADIO_SHOW_POKEMON_TALK;
    case 2:
        return GetScheduledShow(sRadioStation2, LIST_SIZE(sRadioStation2), date->week, time->hour);
    case 3:
        return GetScheduledShow(sRadioStation3, LIST_SIZE(sRadioStation3), date->week, time->hour);
    case 4:
        return RADIO_SHOW_POKE_FLUTE;
    case 5:
        return RADIO_SHOW_UNOWN;
    case 6:
        return RADIO_SHOW_TEAM_ROCKET;
    case 7:
        return RADIO_SHOW_MAHOGANY_SIGNAL;
    }

    return RADIO_SHOW_POKEMON_MUSIC;
}