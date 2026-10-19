#include "../include/types.h"

#include "../include/message.h"
#include "../include/pokemon.h"
#include "../include/sound.h"
#include "../include/summary.h"
#include "../include/system.h"
#include "../include/window.h"

// Ability in the Pokemon summary, Skills page.
//
// 1. The ability box on the top screen shows only "Ability", the name and a "Y" hint (the description is erased from it).
// 2. Y opens a window over the Pokemon sprite with the name and the whole description of the ability. A, B or Y close it.
//
// It is called by Summary_IVEV (asm/other_hook.s) every frame the summary waits for an input.

#define SUMMARY_PAGE_SKILLS 1

#define KEY_A 0x0001
#define KEY_B 0x0002
#define KEY_Y 0x0800

#define SUMMARY_HEAP_ID     19
#define MSG_NARC            27
#define MSG_ABILITY_NAMES   721
#define MSG_ABILITY_DESCS   722
#define SE_BUTTON           0x5E1 // same sound the L/R buttons of the summary use

// Extra windows of the Skills page: list pointer at summary + 0x224, count at summary + 0x228, 0x10 bytes each
#define SUMMARY_ADDL_WINDOWS_OFFSET 0x224
#define SUMMARY_ADDL_COUNT_OFFSET   0x228
#define WINDOW_SIZE                 0x10
#define ADDL_NAME_INDEX             6  // bg 1, x9 y17 w9 h2: name of the ability
#define ADDL_DESC_INDEX             7  // bg 1, x0 y19 w19 h4: description of the ability
#define WINDOW_PIXELS_OFFSET        0xC // pointer to the pixel buffer of a window
#define WINDOW_WIDTH_OFFSET         7
#define WINDOW_HEIGHT_OFFSET        8

// "(Y)" hint, printed on the right side of the name window (the window is 72 pixels wide)
// The codes of the parentheses come from my memory of the game's character table: if another symbol appears, look for
// "(" and ")" in the charmap.txt of hg-engine and put the right codes here
#define CHAR_LPAREN 0x01B9
#define CHAR_RPAREN 0x01BA
#define CHAR_Y      0x0143
#define NAME_WINDOW_PIXELS 72

// Top screen: bg 1. The window covers the Pokemon sprite: x 20-31, y 8-17
// The tiles start right after the last tile used by the windows of this page (0x25D + 19 * 4 = 0x2A9)
#define POPUP_BG        1
#define POPUP_X         20
#define POPUP_Y         8
#define POPUP_WIDTH     12
#define POPUP_HEIGHT    10
#define POPUP_PALETTE   13
#define POPUP_BASE_TILE 0x2AA
#define TILEMAP_FILL_INCLUDES_PALETTE 17

// The sprite of the Pokemon is NOT an object (the ball next to the name is): it is drawn on one of the backgrounds of the top screen.
// SPRITE_BG_MASK is the bit of that background in the display register: 0x0100 = BG0, 0x0400 = BG2, 0x0800 = BG3.
// While the window is open that layer is hidden. 0 = nothing is hidden (use the test below to find the right one)
#define REG_DISPCNT_TOP (*(volatile u32 *)0x04000000)
#define SPRITE_BG_MASK  0x0100  // BG0: found with the layer test

// 1 = the X button hides one layer at a time (BG0, BG2, BG3, all back) on the Skills page, to find which one holds the sprite.
#define SPRITE_LAYER_TEST 0

// Frames to wait after the window is opened before the sprite is hidden: the text reaches the screen a frame later than the
// register write, so hiding the sprite at once leaves an empty moment. If the text is still over the sprite for too long, lower it
#define SPRITE_HIDE_DELAY 2

#define TEXT_COLOR      0x10200 // same colors as the other summary texts: letter 1, shadow 2, background 0
#define TEXT_X          2
#define TITLE_Y         0
#define DESCRIPTION_Y   14
#define LINE_MAX_WIDTH  90      // pixels of a line of the description
#define MEASURE_WIDTH   2048    // width used to measure a string with FontID_String_GetCenterAlignmentX

#define CHAR_SPACE 0x01DE
#define CHAR_EOL   0xE000
#define CHAR_EOS   0xFFFF
#define MAX_CHARS  400

static struct Window sPopupWindow;
static BOOL sPopupOpen;
static u8 sSpriteHideDelay;

void LONG_CALL ScheduleBgTilemapBufferTransfer(void *bgConfig, u8 bgId);

// Width in pixels of the first count characters of the text, measured by the game itself with its font
static u32 GetTextWidth(String *tmp, const u16 *chars, u32 count)
{
    u32 i;

    for (i = 0; i < count; i++) {
        tmp->data[i] = chars[i];
    }

    tmp->data[count] = CHAR_EOS;
    tmp->size = count;

    return MEASURE_WIDTH - 2 * FontID_String_GetCenterAlignmentX(0, tmp, 0, MEASURE_WIDTH);
}

// Same text with new line breaks, so that every line fits in the window. The breaks of the original are ignored
static void WrapText(const u16 *src, String *dst, String *tmp, u16 *line)
{
    u32 lineLen = 0;
    u32 outLen = 0;
    u32 i = 0;

    while (src[i] != CHAR_EOS) {
        u32 wordStart;
        u32 wordLen;
        u32 j;

        while (src[i] == CHAR_SPACE || src[i] == CHAR_EOL) {
            i++;
        }

        if (src[i] == CHAR_EOS) {
            break;
        }

        wordStart = i;
        while (src[i] != CHAR_EOS && src[i] != CHAR_SPACE && src[i] != CHAR_EOL) {
            i++;
        }
        wordLen = i - wordStart;

        if (outLen + lineLen + wordLen + 2 >= MAX_CHARS) {
            break;
        }

        // the line with the new word
        for (j = 0; j < wordLen; j++) {
            line[lineLen + (lineLen != 0) + j] = src[wordStart + j];
        }

        if (lineLen != 0) {
            line[lineLen] = CHAR_SPACE;

            if (GetTextWidth(tmp, line, lineLen + 1 + wordLen) > LINE_MAX_WIDTH) {
                // does not fit: the word starts a new line
                for (j = 0; j < lineLen; j++) {
                    dst->data[outLen++] = line[j];
                }
                dst->data[outLen++] = CHAR_EOL;

                for (j = 0; j < wordLen; j++) {
                    line[j] = src[wordStart + j];
                }
                lineLen = wordLen;
                continue;
            }

            lineLen += 1 + wordLen;
        } else {
            lineLen = wordLen;
        }
    }

    for (i = 0; i < lineLen; i++) {
        dst->data[outLen++] = line[i];
    }

    dst->data[outLen] = CHAR_EOS;
    dst->size = outLen;
}

static void OpenPopup(void *summary, u32 ability)
{
    void *bgConfig = *(void **)summary;
    struct WindowTemplate template = { POPUP_BG, POPUP_X, POPUP_Y, POPUP_WIDTH, POPUP_HEIGHT, POPUP_PALETTE, POPUP_BASE_TILE };
    MsgData *names = NewMsgDataFromNarc(MSGDATA_LOAD_LAZY, MSG_NARC, MSG_ABILITY_NAMES, SUMMARY_HEAP_ID);
    MsgData *descriptions = NewMsgDataFromNarc(MSGDATA_LOAD_LAZY, MSG_NARC, MSG_ABILITY_DESCS, SUMMARY_HEAP_ID);
    String *name = NewString_ReadMsgData(names, ability);
    String *description = NewString_ReadMsgData(descriptions, ability);
    String *wrapped = String_New(MAX_CHARS, SUMMARY_HEAP_ID);
    String *tmp = String_New(MAX_CHARS, SUMMARY_HEAP_ID);
    String *lineBuffer = String_New(MAX_CHARS, SUMMARY_HEAP_ID);

    DestroyMsgData(names);
    DestroyMsgData(descriptions);

    WrapText(description->data, wrapped, tmp, lineBuffer->data);

    // background 0 is transparent, like in the other windows of this screen
    AddWindow(bgConfig, &sPopupWindow, &template);
    FillWindowPixelBuffer(&sPopupWindow, 0);
    AddTextPrinterParameterizedWithColor(&sPopupWindow, 0, name, TEXT_X, TITLE_Y, 0xFF, TEXT_COLOR, NULL);
    AddTextPrinterParameterizedWithColor(&sPopupWindow, 0, wrapped, TEXT_X, DESCRIPTION_Y, 0xFF, TEXT_COLOR, NULL);

    // puts the window on the tilemap and sends its tiles to the screen
    ScheduleWindowCopyToVram(&sPopupWindow);
    ScheduleBgTilemapBufferTransfer(bgConfig, POPUP_BG);

    // the sprite is hidden a few frames later (see Summary_HandleAbilityPopup), when the text is already on the screen
    sSpriteHideDelay = SPRITE_HIDE_DELAY;

    String_Delete(name);
    String_Delete(description);
    String_Delete(wrapped);
    String_Delete(tmp);
    String_Delete(lineBuffer);
    sPopupOpen = TRUE;
}

static void ClosePopup(void *summary)
{
    void *bgConfig = *(void **)summary;

    FillBgTilemapRect(bgConfig, POPUP_BG, 0, POPUP_X, POPUP_Y, POPUP_WIDTH, POPUP_HEIGHT, TILEMAP_FILL_INCLUDES_PALETTE);
    ScheduleBgTilemapBufferTransfer(bgConfig, POPUP_BG);
    RemoveWindow(&sPopupWindow);

    sSpriteHideDelay = 0;
    REG_DISPCNT_TOP |= SPRITE_BG_MASK;
    sPopupOpen = FALSE;
}

// Ability of the Pokemon shown by the summary (it can be above 255, so it is read from the Pokemon and not from the summary's own copy)
static u32 GetSummaryAbility(void *summary)
{
    return GetBoxMonData(Summary_GetPokemonData(summary), MON_DATA_ABILITY, NULL);
}

// TRUE if the pixel buffer of the window has anything drawn in it
static BOOL WindowHasPixels(struct Window *window)
{
    u8 *raw = (u8 *)window;
    u32 *pixels = *(u32 **)(raw + WINDOW_PIXELS_OFFSET);
    u32 words = raw[WINDOW_WIDTH_OFFSET] * raw[WINDOW_HEIGHT_OFFSET] * 8; // a tile is 32 bytes = 8 words
    u32 i;

    if (pixels == NULL) {
        return FALSE;
    }

    for (i = 0; i < words; i++) {
        if (pixels[i] != 0) {
            return TRUE;
        }
    }

    return FALSE;
}

// The summary draws the name and the description of the ability when it shows a Pokemon. Right after that the description
// window has pixels in it: that is the moment to erase it and to add the "Y" hint. Once erased, nothing is done until the
// summary draws it again (another Pokemon, another page).
static void ReshapeAbilityBox(void *summary)
{
    u8 *list = *(u8 **)((u8 *)summary + SUMMARY_ADDL_WINDOWS_OFFSET);
    u32 count = *(u32 *)((u8 *)summary + SUMMARY_ADDL_COUNT_OFFSET);
    struct Window *name;
    struct Window *description;
    String *hint;
    int hintX;

    if (list == NULL || count <= ADDL_DESC_INDEX) {
        return;
    }

    name = (struct Window *)(list + WINDOW_SIZE * ADDL_NAME_INDEX);
    description = (struct Window *)(list + WINDOW_SIZE * ADDL_DESC_INDEX);

    if (!WindowHasPixels(description)) {
        return;
    }

    FillWindowPixelBuffer(description, 0);
    ScheduleWindowCopyToVram(description);

    hint = String_New(8, SUMMARY_HEAP_ID);
    hint->data[0] = CHAR_LPAREN;
    hint->data[1] = CHAR_Y;
    hint->data[2] = CHAR_RPAREN;
    hint->data[3] = CHAR_EOS;
    hint->size = 3;

    // right aligned in the name window
    hintX = NAME_WINDOW_PIXELS - (MEASURE_WIDTH - 2 * FontID_String_GetCenterAlignmentX(0, hint, 0, MEASURE_WIDTH));

    AddTextPrinterParameterizedWithColor(name, 0, hint, hintX, 0, 0xFF, TEXT_COLOR, NULL);
    ScheduleWindowCopyToVram(name);

    String_Delete(hint);
}

// Mode shown by the stats page: 0 = Stats, 1 = EV, 2 = IV (the numbers Summary_ChangeStatScreenState expects)
static u8 sStatMode;

// Called by Summary_Entry_Hook right after the stats page is drawn: it always starts as Stats
void Summary_ResetStatMode(void)
{
    sStatMode = 0;
}

// Called by Summary_IVEV when L or R is pressed on the Skills page. The modes rotate:
// step 1 (R): Stats -> EV -> IV -> Stats
// step 2 (L): Stats -> IV -> EV -> Stats
void Summary_RotateStatScreen(struct SummaryState *summary, u32 step)
{
    sStatMode += step;

    if (sStatMode >= 3) {
        sStatMode -= 3;
    }

    Summary_ChangeStatScreenState(summary, sStatMode);
}

// Called by Summary_IVEV every frame the summary waits for an input. page is the current page of the summary.
// Returns TRUE when the input was used here, so the summary does not handle it
BOOL Summary_HandleAbilityPopup(void *summary, int page)
{
    u32 newKeys = gSystem.newKeys;

    if (sPopupOpen) {
        if (sSpriteHideDelay != 0) {
            sSpriteHideDelay--;

            if (sSpriteHideDelay == 0) {
                REG_DISPCNT_TOP &= ~SPRITE_BG_MASK;
            }
        }

        if (newKeys & (KEY_A | KEY_B | KEY_Y)) {
            PlaySE(SE_BUTTON);
            ClosePopup(summary);
        }

        return TRUE;    // while the window is open nothing else can be pressed
    }

    if (page != SUMMARY_PAGE_SKILLS) {
        return FALSE;
    }

    ReshapeAbilityBox(summary);

#if SPRITE_LAYER_TEST
    if (newKeys & 0x0400) {    // X
        static const u16 sLayers[] = { 0x0100, 0x0400, 0x0800 };   // BG0, BG2, BG3
        static u32 sStep;
        static u32 sSaved;

        if (sStep == 0) {
            sSaved = REG_DISPCNT_TOP & 0x1F00;
        }

        // everything back as it was, then hides the layer of this step (the 4th press leaves everything visible)
        REG_DISPCNT_TOP = (REG_DISPCNT_TOP & ~0x1F00) | sSaved;

        if (sStep < 3) {
            REG_DISPCNT_TOP &= ~sLayers[sStep];
        }

        sStep = (sStep + 1) % 4;
    }
#endif

    if (newKeys & KEY_Y) {
        u32 ability = GetSummaryAbility(summary);

        if (ability != 0) {
            PlaySE(SE_BUTTON);
            OpenPopup(summary, ability);
            return TRUE;
        }
    }

    return FALSE;
}