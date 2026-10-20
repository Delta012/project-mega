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
// - Apricorn trees, Berry Pots, Pokégear and every other thing that checked the time has been converted to use the new system too.
// ============================================================================================================

// ============================================================================================================
// SAVE VARIABLES
// ============================================================================================================

// These are free script variables of the save file. No vanilla script uses them.
#define GAME_CLOCK_ELAPSED_SECONDS_VAR 0x40E4   // Seconds played in the current loop (a loop = morning + day + evening + night)

#define GAME_CLOCK_VARS_BASE           0x4059
#define GAME_CLOCK_APRICORN_DAYS_VAR   GAME_CLOCK_VARS_BASE                 // Game days passed since the apricorn trees grew back
#define GAME_CLOCK_BERRY_CLOCK_VAR     (GAME_CLOCK_VARS_BASE + 1)           // Seconds played since the save was created (uses 2 variables)
#define GAME_CLOCK_BERRY_STAMP_VAR     (GAME_CLOCK_VARS_BASE + 3)           // For each Berry Pot: the berry clock when it was last updated (2 variables per pot)
#define GAME_CLOCK_SEASON_VAR          (GAME_CLOCK_BERRY_STAMP_VAR + MAX_BERRY_POT * 2) // The season of the current loop (1 = spring ... 4 = winter, 0 = not set)
#define GAME_CLOCK_VARS_END            (GAME_CLOCK_SEASON_VAR + 1)

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

CLOCK_STATIC_ASSERT(ClockNewGameCycleValid, GAME_CLOCK_NEW_GAME_CYCLE >= 0 && GAME_CLOCK_NEW_GAME_CYCLE < CLOCK_CYCLE_COUNT);
CLOCK_STATIC_ASSERT(ClockVarsInSaveRange, GAME_CLOCK_VARS_BASE >= 0x4020 && GAME_CLOCK_VARS_END - 1 <= 0x416F);
CLOCK_STATIC_ASSERT(ClockVarsDistinct, GAME_CLOCK_ELAPSED_SECONDS_VAR < GAME_CLOCK_VARS_BASE || GAME_CLOCK_ELAPSED_SECONDS_VAR >= GAME_CLOCK_VARS_END);
CLOCK_STATIC_ASSERT(ApricornRegrowDaysValid, APRICORN_REGROW_DAYS >= 1 && APRICORN_REGROW_DAYS <= 0xFFFF);
CLOCK_STATIC_ASSERT(BerryRegrowDaysValid, BERRY_REGROW_DAYS >= 1);

// ============================================================================================================
// TABLES
// ============================================================================================================

struct ClockCycle {
    u32 CycleStartSecond;       // The game second of the day when this part starts
    u32 CycleInGameSeconds;     // How many game seconds this part lasts
};

// The game hours of each part of the day. They match the vanilla tables, so the colors, music and wild Pokemon change at the right moment.
static const struct ClockCycle sClockCycles[CLOCK_CYCLE_COUNT] = {
    [CLOCK_CYCLE_MORNING] = {  4 * 3600,  6 * 3600 }, // 04:00 - 10:00
    [CLOCK_CYCLE_DAY]     = { 10 * 3600,  7 * 3600 }, // 10:00 - 17:00
    [CLOCK_CYCLE_EVENING] = { 17 * 3600,  3 * 3600 }, // 17:00 - 20:00
    [CLOCK_CYCLE_NIGHT]   = { 20 * 3600,  8 * 3600 }, // 20:00 - 04:00
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

// The season the current loop uses. It is saved, so a loop keeps the same season until it ends.
static u32 GetLoopSeason(void)
{
    u32 stored = GetScriptVar(GAME_CLOCK_SEASON_VAR);

    // Nothing saved yet (new game): use today's season
    if (stored == 0 || stored > CLOCK_SEASON_COUNT) {
        stored = GetRealSeason() + 1;
        SetScriptVar(GAME_CLOCK_SEASON_VAR, stored);
    }

    return stored - 1;
}

// How many seconds of the current loop you have played. This is the core of the clock feature.
// It adds the time that passed since the last call and saves it.
// A loop is always 24 game hours, so when a loop ends and the season changes, the time does not jump.
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

            // The loop is over: start the next one, with the season of today
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

// The game second of the day (0 - 86399). 0 is midnight, 43200 is noon.
static u32 GetGameSecondOfDay(void)
{
    u32 elapsedSeconds = GetCycleElapsedSeconds();
    u32 gameSecond = 0;
    int i;

    // The loop starts at the part of the day where a new game begins.
    // Go through the parts one by one, until we find the one that contains the played time.
    for (i = 0; i < CLOCK_CYCLE_COUNT; i++) {
        int index = (GAME_CLOCK_NEW_GAME_CYCLE + i) % CLOCK_CYCLE_COUNT;
        const struct ClockCycle *cycle = &sClockCycles[index];
        u32 cycleSeconds = sSeasonCycleSeconds[sLoopSeason][index];

        if (elapsedSeconds < cycleSeconds) {
            // Found it. Place the time inside the game hours of this part of the day.
            gameSecond = (cycle->CycleStartSecond + elapsedSeconds * cycle->CycleInGameSeconds / cycleSeconds) % SECONDS_PER_DAY;
            break;
        }

        elapsedSeconds -= cycleSeconds;
    }

    if (SaveBlock2_get() != NULL) {
        UpdateApricornTrees(gameSecond);
    }

    return gameSecond;
}

// ============================================================================================================
// FUNCTIONS THAT REPLACE VANILLA ONES (see ~\hooks)
// ============================================================================================================

// Gives the hour, minute and second. Used by wild encounters, evolutions, music and Gracidea.
void LONG_CALL GF_RTC_CopyTime_Detached(struct RTCTime *time)
{
    u32 gameSecond = GetGameSecondOfDay();

    time->hour = gameSecond / 3600;
    time->minute = (gameSecond % 3600) / 60;
    time->second = gameSecond % 60;
}

// Gives the second of the day. Used by the colors (tint) of the map.
s32 LONG_CALL GF_RTC_TimeToSec_Detached(void)
{
    return (s32)GetGameSecondOfDay();
}

// Gives morning / day / evening / night. Used by battle backgrounds, the GetTimeOfDay script command and NPC trades.
// The vanilla function is fine: it already asks GF_RTC_CopyTime, which is the game clock now.
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

// Gives only the hour.
u32 LONG_CALL Field_GetHour_Detached(void *fieldSystem)
{
    (void)fieldSystem;
    return GetGameSecondOfDay() / 3600;
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
// The Pokégear now showsshow the in-game time. The weekday is still the real one.
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
    GF_RTC_CopyTime_Detached(&time);

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
// HOW TO CHANGE THE SCHEDULE:
// Each station has a short list. Every line says: "from this game hour, play this show".
// A show plays until the hour of the next line. The last line plays until midnight.
// Rules: the first line must start at hour 0, and the hours must go up (0, 8, 16 ... never 8, 0).
// Example: { 0, SEARCH }, { 8, DRAMA }, { 16, BUENA } = Search Party from 00:00, Radio Drama from 08:00, Buena's Password from 16:00.
// To add a show, add a line. To remove one, delete its line.
// The show is chosen when you tune the station and when a show ends. The radio never changes show in the middle of one.
// ============================================================================================================

// Show numbers used by the radio (the game uses these numbers)
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
 
// Short names, to keep the lists easy to read
#define TRAINERS RADIO_SHOW_TRAINER_PROFILES
#define TOWN     RADIO_SHOW_THAT_TOWN_THESE_PEOPLE
#define SEARCH   RADIO_SHOW_POKEMON_SEARCH_PARTY
#define DRAMA    RADIO_SHOW_SERIAL_RADIO_DRAMA
#define BUENA    RADIO_SHOW_BUENAS_PASSWORD
 
struct RadioSlot {
    u8 startHour;   // The game hour (0 - 23) when the show starts
    u8 show;
};
 
// SCHEDULE
 
// Station 3
static const struct RadioSlot sRadioStation2[] = {
    {  0, TRAINERS },   // 00:00 - 11:59
    { 12, TOWN },       // 12:00 - 23:59
};
 
// Station 4
static const struct RadioSlot sRadioStation3[] = {
    {  0, SEARCH },     // 00:00 - 07:59
    {  8, DRAMA },      // 08:00 - 15:59
    { 16, BUENA },      // 16:00 - 23:59
};
 
// Gives the show of a list for an hour: the last line that already started
static u8 GetScheduledShow(const struct RadioSlot *list, u32 count, u32 hour)
{
    u8 show = list[0].show;
    u32 i;
 
    for (i = 0; i < count; i++) {
        if (list[i].startHour <= hour) {
            show = list[i].show;
        }
    }
 
    return show;
}
 
#define LIST_SIZE(list) (sizeof(list) / sizeof((list)[0]))
 
// Where things are inside the radio data (RadioShow)
#define RADIO_DATE_OFFSET       0x28    // RTCDate: the show reads the weekday from here
#define RADIO_TIME_OFFSET       0x38    // RTCTime
#define RADIO_FLAGS_OFFSET      0x66    // u8 with several 1 bit flags
#define RADIO_FLAG_COMMERCIALS  0x10    // The flag "play the commercials next"
 
// Replaces RadioShow_TranslateStationID: gives the show to play for a station of the dial (0 - 7)
u8 LONG_CALL RadioShow_TranslateStationID_Detached(void *radioShow, int station)
{
    u8 *radio = (u8 *)radioShow;
    struct RTCTime *time = (struct RTCTime *)(radio + RADIO_TIME_OFFSET);
 
    if (station >= 8) {
        station = 0;
    }
 
    // The previous show asked for the commercials: play them once
    if (radio[RADIO_FLAGS_OFFSET] & RADIO_FLAG_COMMERCIALS) {
        radio[RADIO_FLAGS_OFFSET] &= ~RADIO_FLAG_COMMERCIALS;
        return RADIO_SHOW_COMMERCIALS;
    }
 
    // The date is the real one (the shows use the weekday), the time is the game time
    GF_RTC_CopyDate((struct RTCDate *)(radio + RADIO_DATE_OFFSET));
    GF_RTC_CopyTime_Detached(time);
 
    switch (station) {
    case 0:
        return RADIO_SHOW_POKEMON_MUSIC;
    case 1:
        return RADIO_SHOW_POKEMON_TALK;
    case 2:
        return GetScheduledShow(sRadioStation2, LIST_SIZE(sRadioStation2), time->hour);
    case 3:
        return GetScheduledShow(sRadioStation3, LIST_SIZE(sRadioStation3), time->hour);
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