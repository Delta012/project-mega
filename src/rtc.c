#include "config.h"
#include "types.h"

#include "save.h"

#ifdef ACCELERATED_GAME_CLOCK

#define SECONDS_PER_DAY 86400

/*\
 *  A full in-game day (24h) completes in 72 irl minutes (4320s):
 *
 *      Morning:  18 irl minutes  (1080s) > 6 in-game hours  (04:00–10:00)
 *      Day:      18 irl minutes  (1080s) > 7 in-game hours  (10:00–17:00)
 *      Evening:   3 irl minutes   (180s) > 3 in-game hours  (17:00–20:00)
 *      Night:    33 irl minutes  (1980s) > 8 in-game hours  (20:00–04:00)
 *
 *  The clock is divided into repeating 4320-second cycles. Within each cycle the
 *  game time advances through the four periods at the rates above. Because the game-second
 *  totals per period (21600 + 25200 + 10800 + 28800 = 86400) equal a full day, the mapping
 *  is continuous across cycle boundaries.
 *
 *  The clock is driven by the save file's play time:
 *    - it only advances while the game is running
 *    - it is saved and loaded together with the save, so the time resumes where you stopped
 *    - a new game starts at 10:00 (NEW_GAME_START_CYCLE_POS)
\*/

#define CYCLE_SECONDS        4320

#define MORNING_SECONDS  1080
#define DAY_SECONDS      1080
#define EVENING_SECONDS   180
#define NIGHT_SECONDS    1980

#define MORNING_START_SEC    ( 4 * 3600)   /* 14400 */
#define DAY_START_SEC        (10 * 3600)   /* 36000 */
#define EVENING_START_SEC    (17 * 3600)   /* 61200 */
#define NIGHT_START_SEC      (20 * 3600)   /* 72000 */

#define MORNING_GAME_SECONDS ( 6 * 3600)   /* 21600 */
#define DAY_GAME_SECONDS     ( 7 * 3600)   /* 25200 */
#define EVENING_GAME_SECONDS ( 3 * 3600)   /* 10800 */
#define NIGHT_GAME_SECONDS   ( 8 * 3600)   /* 28800 */

/* New games start at 10:00 = beginning of the Day period */
#define NEW_GAME_START_CYCLE_POS  MORNING_SECONDS   /* 1080 */

/*
 * Delta tracking state to filter out IGT inflation from game closure.
 * These variables persist in memory during the game session but reset on power-off.
 */
static u32 sAccumulatedSeconds = 0;
static u32 sLastSeenIGT = 0;
static BOOL sInitialized = FALSE;

/**
 *  @brief get the IGT (In-Game Time) block from the save
 *
 *  Implement this to return a pointer to your save's IGT struct.
 *  The IGT struct has fields: hours (u16), minutes (u8), seconds (u8)
 *
 *  @return pointer to the save's IGT struct, or NULL if no save is available.
 */
static struct IGT *GetSavePlayTime(void)
{
    /* TODO: Implement this based on your save structure */
    /* Example:
     * void *saveData = SaveBlock2_get();
     * // Get pointer to PLAYERDATA which contains the IGT
     * struct PLAYERDATA *playerData = ...; // your accessor here
     * return &playerData->igt;
     */
    return NULL;
}

/**
 *  @brief total seconds of ACTIVE play time (excludes time game was closed)
 *
 *  Uses delta tracking: only accumulates small changes between readings.
 *  Large jumps (> 5 seconds) indicate the game was closed and the IGT
 *  was inflated by the vanilla catch-up mechanism.
 *
 *  @return accumulated seconds of actual play time
 */
static u32 GetPlayedSeconds(void)
{
    struct IGT *pt = GetSavePlayTime();

    if (pt == NULL) {
        return sAccumulatedSeconds;
    }

    u32 currentIGT = (u32)pt->hours   * 3600
                   + (u32)pt->minutes * 60
                   + (u32)pt->seconds;

    if (!sInitialized) {
        /*
         *  First call after boot/load.
         *  The IGT might include catch-up time from when the game was closed.
         *  We accept this as the baseline. From this point forward, we only
         *  add small deltas (actual play time), so the clock will be correct
         *  going forward even if the initial value is slightly off.
         *
         *  To fully eliminate the initial offset, you would need to either:
         *    - Hook the save loading to read IGT before catch-up
         *    - Store a separate counter in the save
         *    - Use the RTC to subtract the estimated catch-up
         */
        sAccumulatedSeconds = currentIGT;
        sLastSeenIGT = currentIGT;
        sInitialized = TRUE;
        return sAccumulatedSeconds;
    }

    /*
     *  Calculate delta since last reading.
     *  During gameplay this is typically 0-2 seconds.
     *  After closing the game, this will be minutes/hours.
     */
    u32 delta = currentIGT - sLastSeenIGT;

    if (delta <= 5) {
        /* Small delta: game was running normally, accumulate it */
        sAccumulatedSeconds += delta;
    }
    /* else: Large delta = game was closed and reopened.
     *       The IGT was inflated by catch-up. Don't add it.
     *       Just update our reference point for next time. */

    sLastSeenIGT = currentIGT;
    return sAccumulatedSeconds;
}

/**
 *  @brief get the accelerated in-game second of the day (0-86399)
 *
 *  Uses a piecewise linear mapping so each time-of-day period lasts a specific
 *  number of real-life minutes of play time. The mapping is a pure function of
 *  the save's play time, so there is no extra state to save: it freezes while the
 *  game is closed and resumes from the saved position on load.
 *
 *  GF_RTC_CopyDate and GF_RTC_CopyDateTime are not hooked, so they still read
 *  the real clock; dates, weekdays and seasons keep following the real calendar.
 */
static u32 GetGameSecondOfDay(void)
{
    /* Position within the 72-minute repeating cycle */
    u32 cyclePos = (NEW_GAME_START_CYCLE_POS + GetPlayedSeconds()) % CYCLE_SECONDS;
    u32 gameSecond;

    if (cyclePos < MORNING_SECONDS) {
        /*
         *  Morning: 04:00 – 10:00
         *  IRL seconds [0, 1080)  →  game seconds [14400, 36000)
         *  Speed: 21600 / 1080 = 20 game-seconds per IRL-second
         */
        gameSecond = MORNING_START_SEC
                   + cyclePos * MORNING_GAME_SECONDS / MORNING_SECONDS;

    } else if (cyclePos < MORNING_SECONDS + DAY_SECONDS) {
        /*
         *  Day: 10:00 – 17:00
         *  IRL seconds [1080, 2160)  →  game seconds [36000, 61200)
         *  Speed: 25200 / 1080 ≈ 23.33 game-seconds per IRL-second
         */
        u32 elapsed = cyclePos - MORNING_SECONDS;
        gameSecond = DAY_START_SEC
                   + elapsed * DAY_GAME_SECONDS / DAY_SECONDS;

    } else if (cyclePos < MORNING_SECONDS + DAY_SECONDS + EVENING_SECONDS) {
        /*
         *  Evening: 17:00 – 20:00
         *  IRL seconds [2160, 2340)  →  game seconds [61200, 72000)
         *  Speed: 10800 / 180 = 60 game-seconds per IRL-second
         */
        u32 elapsed = cyclePos - MORNING_SECONDS - DAY_SECONDS;
        gameSecond = EVENING_START_SEC
                   + elapsed * EVENING_GAME_SECONDS / EVENING_SECONDS;

    } else {
        /*
         *  Night: 20:00 – 04:00  (wraps around midnight)
         *  IRL seconds [2340, 4320)  →  game seconds [72000, 100800) mod 86400
         *  Speed: 28800 / 1980 ≈ 14.55 game-seconds per IRL-second
         */
        u32 elapsed = cyclePos - MORNING_SECONDS - DAY_SECONDS - EVENING_SECONDS;
        gameSecond = (NIGHT_START_SEC
                    + elapsed * NIGHT_GAME_SECONDS / NIGHT_SECONDS)
                    % SECONDS_PER_DAY;
    }

    return gameSecond;
}

/**
 *  @brief replacement for GF_RTC_CopyTime: encounter slots, evolutions, route music, Gracidea
 *
 *  @param time output: the in-game time
 */
void LONG_CALL GF_RTC_CopyTime_Accelerated(struct RTCTime *time)
{
    u32 gameSecondOfDay = GetGameSecondOfDay();

    time->hour   = gameSecondOfDay / 3600;
    time->minute = (gameSecondOfDay % 3600) / 60;
    time->second = gameSecondOfDay % 60;
}

/**
 *  @brief replacement for GF_RTC_TimeToSec: the area light (the day/night tint of the overworld)
 *         reads the time through this
 *
 *  @return the in-game second of the day
 */
s32 LONG_CALL GF_RTC_TimeToSec_Accelerated(void)
{
    return (s32)GetGameSecondOfDay();
}

/**
 *  @brief replacement for Field_GetTimeOfDay: battle backgrounds, the GetTimeOfDay script
 *         command and NPC trades read the time through this.
 *         The vanilla function reads the real time that was cached in the save file.
 *
 *  GF_RTC_GetTimeOfDay goes through the hooked GF_RTC_CopyTime, so it already uses
 *  the accelerated clock.
 *
 *  @param fieldSystem unused
 *  @return time of day
 */
TIMEOFDAY LONG_CALL Field_GetTimeOfDay_Accelerated(void *fieldSystem)
{
    (void)fieldSystem;
    return GF_RTC_GetTimeOfDay();
}

/**
 *  @brief replacement for Field_GetTimeOfDayWildParam: trainer phone rematches
 *
 *  @param fieldSystem unused
 *  @return wild time of day parameter
 */
TimeOfDayWildParam LONG_CALL Field_GetTimeOfDayWildParam_Accelerated(void *fieldSystem)
{
    (void)fieldSystem;
    return GF_RTC_GetTimeOfDayWildParam();
}

/**
 *  @brief replacement for Field_GetHour: scripts that check the exact hour
 *
 *  @param fieldSystem unused
 *  @return in-game hour (0-23)
 */
u32 LONG_CALL Field_GetHour_Accelerated(void *fieldSystem)
{
    (void)fieldSystem;
    return GetGameSecondOfDay() / 3600;
}

#endif /* ACCELERATED_GAME_CLOCK */