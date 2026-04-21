#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef NUMWORKS

extern "C"
{
#include "doomdef.h"
#include "doomtype.h"
#include "d_main.h"
#include "d_event.h"
#include "lprintf.h"
#include "m_cheat.h"
#include "z_zone.h"
}

#include "i_system_e32.h"

extern "C"
{
#include <eadk.h>
}

static const unsigned int kSourceWidth = SCREENWIDTH * 2;
static const unsigned int kSourceHeight = SCREENHEIGHT;
static const unsigned int kSourceStride = SCREENWIDTH * 2;

static byte s_palette[256 * 3];
static eadk_color_t s_palette565[256];
static const byte* s_palette_source = NULL;
static byte s_indexed_frame[SCREENWIDTH * SCREENHEIGHT * 2];
static eadk_color_t s_rgb565_line[EADK_SCREEN_WIDTH];
static unsigned short s_scale_x[EADK_SCREEN_WIDTH];
static unsigned short s_scale_y[EADK_SCREEN_HEIGHT];
static eadk_keyboard_state_t s_prev_keyboard_state = 0;

static const int kDebugLineCount = 6;
static const int kDebugLineWidth = 63;
static char s_debug_lines[kDebugLineCount][kDebugLineWidth + 1];
static int s_debug_line_count = 0;
static bool s_debug_overlay_visible = false;
static bool s_debug_choice_done = false;
static bool s_startup_options_done = false;
static bool s_textured_planes_enabled = true;
static bool s_visual_extras_enabled = true;

static const int kFontWidth = 7;
static const int kLineHeight = 18;
static const int kOverlayTopPadding = 2;

static void DrawDebugOverlay(void);
static void DrawZoneOverlay(void);
static void ShowDebugChoicePrompt(void);
static void ShowStartupOptionsPrompt(void);
static void BuildFullscreenScaleLUTs(void);

static void BuildFullscreenScaleLUTs(void)
{
    for (unsigned int x = 0; x < EADK_SCREEN_WIDTH; x++)
    {
        unsigned int sx = (x * kSourceWidth) / EADK_SCREEN_WIDTH;
        if (sx >= kSourceWidth)
            sx = kSourceWidth - 1;
        s_scale_x[x] = (unsigned short)sx;
    }

    for (unsigned int y = 0; y < EADK_SCREEN_HEIGHT; y++)
    {
        unsigned int sy = (y * kSourceHeight) / EADK_SCREEN_HEIGHT;
        if (sy >= kSourceHeight)
            sy = kSourceHeight - 1;
        s_scale_y[y] = (unsigned short)sy;
    }
}

static void ClearOverlayBand(void)
{
    eadk_rect_t overlay_rect;
    overlay_rect.x = 0;
    overlay_rect.y = 0;
    overlay_rect.width = EADK_SCREEN_WIDTH;
    overlay_rect.height = (uint16_t)(kOverlayTopPadding + (kDebugLineCount * kLineHeight));
    eadk_display_push_rect_uniform(overlay_rect, eadk_color_black);
}

static void DrawDebugOverlay(void)
{
    const int x = 2;
    const int y0 = kOverlayTopPadding;

    for (int i = 0; i < s_debug_line_count; i++)
    {
        const int y = y0 + i * kLineHeight;
        eadk_display_draw_string(s_debug_lines[i], (eadk_point_t){(uint16_t)x, (uint16_t)y}, false, eadk_color_white, eadk_color_black);
    }
}

static void DrawZoneOverlay(void)
{
    char overlay[32];
    unsigned int used_zone = Z_GetAllocatedMemory();
    int len;
    int x;

    snprintf(overlay, sizeof(overlay), "Z:%u", used_zone);
    len = (int)strlen(overlay);
    x = EADK_SCREEN_WIDTH - (len * kFontWidth) - 2;

    if (x < 0)
    {
        x = 0;
    }

    eadk_display_draw_string(overlay, (eadk_point_t){(uint16_t)x, (uint16_t)kOverlayTopPadding}, false, eadk_color_white, eadk_color_black);
}

static void DrawWrappedText(const char* text, int x, int y, int max_width, int max_height, eadk_color_t fg, eadk_color_t bg)
{
    if (text == NULL || max_width <= 0 || max_height <= 0)
    {
        return;
    }

    const int max_chars = max_width / kFontWidth;
    const int max_lines = max_height / kLineHeight;

    if (max_chars <= 0 || max_lines <= 0)
    {
        return;
    }

    int line = 0;
    const char* p = text;

    while (*p != '\0' && line < max_lines)
    {
        while (*p == ' ')
        {
            p++;
        }

        if (*p == '\0')
        {
            break;
        }

        if (*p == '\n')
        {
            line++;
            p++;
            continue;
        }

        char line_buf[128];
        int len = 0;
        const char* last_space = NULL;
        int last_space_len = -1;

        while (*p != '\0' && *p != '\n' && len < max_chars && len < (int)sizeof(line_buf) - 1)
        {
            line_buf[len] = *p;
            if (*p == ' ')
            {
                last_space = p;
                last_space_len = len;
            }
            len++;
            p++;
        }

        if (*p != '\0' && *p != '\n' && len == max_chars && last_space != NULL)
        {
            p = last_space + 1;
            len = last_space_len;
        }

        while (len > 0 && line_buf[len - 1] == ' ')
        {
            len--;
        }

        line_buf[len] = '\0';

        if (len > 0)
        {
            eadk_display_draw_string(line_buf, (eadk_point_t){(uint16_t)x, (uint16_t)(y + line * kLineHeight)}, false, fg, bg);
            line++;
        }

        if (*p == '\n')
        {
            p++;
        }
    }
}

static inline eadk_color_t RGB888To565(unsigned int r, unsigned int g, unsigned int b)
{
    return (eadk_color_t)(((r & 0xF8U) << 8) | ((g & 0xFCU) << 3) | (b >> 3));
}

static void RebuildPalette565(const byte* palette)
{
    for (unsigned int i = 0; i < 256; i++)
    {
        const unsigned int p = i * 3;
        s_palette565[i] = RGB888To565(palette[p + 0], palette[p + 1], palette[p + 2]);
    }
}

static void PostKeyEvent(evtype_t type, int key)
{
    event_t ev;
    ev.type = type;
    ev.data1 = key;
    D_PostEvent(&ev);
}

static void ProcessKeyTransition(eadk_keyboard_state_t current, eadk_keyboard_state_t previous, eadk_key_t key, int doom_key)
{
    bool is_down = eadk_keyboard_key_down(current, key);
    bool was_down = eadk_keyboard_key_down(previous, key);

    if (is_down && !was_down)
    {
        // snprintf(msg, sizeof(msg), "[INPUT] keydown %s (%d)", DoomKeyName(doom_key), doom_key);
        // lprintf(LO_ALWAYS, "%s", msg);
        // I_DebugLog_e32(msg);
        PostKeyEvent(ev_keydown, doom_key);
    }
    else if (!is_down && was_down)
    {
        // snprintf(msg, sizeof(msg), "[INPUT] keyup %s (%d)", DoomKeyName(doom_key), doom_key);
        // lprintf(LO_ALWAYS, "%s", msg);
        // I_DebugLog_e32(msg);
        PostKeyEvent(ev_keyup, doom_key);
    }
}

static bool ConsumeCheatHotkeys(eadk_keyboard_state_t current, eadk_keyboard_state_t previous)
{
    bool skip_now = eadk_keyboard_key_down(current, eadk_key_shift)
        && eadk_keyboard_key_down(current, eadk_key_exe);
    bool skip_prev = eadk_keyboard_key_down(previous, eadk_key_shift)
        && eadk_keyboard_key_down(previous, eadk_key_exe);

    bool god_now = eadk_keyboard_key_down(current, eadk_key_alpha)
        && eadk_keyboard_key_down(current, eadk_key_exe);
    bool god_prev = eadk_keyboard_key_down(previous, eadk_key_alpha)
        && eadk_keyboard_key_down(previous, eadk_key_exe);

    if (skip_now && !skip_prev)
    {
        C_TriggerExitLevelCheat();
    }

    if (god_now && !god_prev)
    {
        C_TriggerGodCheat();
    }

    return skip_now || god_now;
}

static void WaitForNoKeys(void)
{
    while (eadk_keyboard_scan() != 0)
    {
        eadk_timing_msleep(10);
    }
}

static void WaitForAnyKeyPress(void)
{
    while (eadk_keyboard_scan() == 0)
    {
        eadk_timing_msleep(10);
    }
}

static void DrawStartupOptionsScreen(int selected_item)
{
    char line_textured[64];
    char line_extras[64];

    snprintf(line_textured, sizeof(line_textured), "%c Textured floors/ceilings: %s",
             selected_item == 0 ? '>' : ' ',
             s_textured_planes_enabled ? "ON" : "OFF");
    snprintf(line_extras, sizeof(line_extras), "%c Lights & decorative objects: %s",
             selected_item == 1 ? '>' : ' ',
             s_visual_extras_enabled ? "ON" : "OFF");

    eadk_display_wait_for_vblank();

    eadk_display_push_rect_uniform(eadk_screen_rect, eadk_color_black);
    DrawWrappedText(
        "Startup options:\n"
        "UP/DOWN: select\n"
        "OK or BACK: toggle\n"
        "EXE: continue",
        4,
        8,
        EADK_SCREEN_WIDTH - 8,
        84,
        eadk_color_white,
        eadk_color_black);
    eadk_display_draw_string(line_textured, (eadk_point_t){6, 110}, false, eadk_color_white, eadk_color_black);
    eadk_display_draw_string(line_extras, (eadk_point_t){6, 128}, false, eadk_color_white, eadk_color_black);
}

static void ShowStartupOptionsPrompt(void)
{
    if (s_startup_options_done)
    {
        return;
    }

    s_startup_options_done = true;

    int selected_item = 0;
    eadk_keyboard_state_t prev_keys = 0;

    WaitForNoKeys();

    while (true)
    {
        eadk_keyboard_state_t keys;
        bool up_now, down_now, ok_now, back_now, exe_now;
        bool up_prev, down_prev, ok_prev, back_prev, exe_prev;

        DrawStartupOptionsScreen(selected_item);
        keys = eadk_keyboard_scan();

        up_now = eadk_keyboard_key_down(keys, eadk_key_up);
        down_now = eadk_keyboard_key_down(keys, eadk_key_down);
        ok_now = eadk_keyboard_key_down(keys, eadk_key_ok);
        back_now = eadk_keyboard_key_down(keys, eadk_key_back);
        exe_now = eadk_keyboard_key_down(keys, eadk_key_exe);

        up_prev = eadk_keyboard_key_down(prev_keys, eadk_key_up);
        down_prev = eadk_keyboard_key_down(prev_keys, eadk_key_down);
        ok_prev = eadk_keyboard_key_down(prev_keys, eadk_key_ok);
        back_prev = eadk_keyboard_key_down(prev_keys, eadk_key_back);
        exe_prev = eadk_keyboard_key_down(prev_keys, eadk_key_exe);

        if ((up_now && !up_prev) || (down_now && !down_prev))
        {
            selected_item ^= 1;
        }

        if ((ok_now && !ok_prev) || (back_now && !back_prev))
        {
            if (selected_item == 0)
                s_textured_planes_enabled = !s_textured_planes_enabled;
            else
                s_visual_extras_enabled = !s_visual_extras_enabled;
        }

        if (exe_now && !exe_prev)
        {
            break;
        }

        prev_keys = keys;
        eadk_timing_msleep(10);
    }

    WaitForNoKeys();

    lprintf(LO_ALWAYS, "[NUMWORKS] Startup options: textured=%s extras=%s",
            s_textured_planes_enabled ? "ON" : "OFF",
            s_visual_extras_enabled ? "ON" : "OFF");
}

static void ShowDebugChoicePrompt(void)
{
    if (s_debug_choice_done)
    {
        return;
    }

    s_debug_choice_done = true;

    eadk_display_push_rect_uniform(eadk_screen_rect, eadk_color_black);
    DrawWrappedText(
        "Debug overlay default:\n"
        "EXE: show logs\n"
        "OK / BACK: hide logs\n"
        "\n"
        "Hotkey in game:\n"
        "SHIFT+ALPHA toggles\n"
        "SHIFT+EXE exits level\n"
        "ALPHA+EXE god mode",
        4,
        8,
        EADK_SCREEN_WIDTH - 8,
        EADK_SCREEN_HEIGHT - 16,
        eadk_color_white,
        eadk_color_black);
    eadk_display_wait_for_vblank();

    WaitForNoKeys();

    while (true)
    {
        eadk_keyboard_state_t keys = eadk_keyboard_scan();
        if (eadk_keyboard_key_down(keys, eadk_key_exe))
        {
            s_debug_overlay_visible = true;
            break;
        }
        if (eadk_keyboard_key_down(keys, eadk_key_back) || eadk_keyboard_key_down(keys, eadk_key_ok))
        {
            s_debug_overlay_visible = false;
            break;
        }
        eadk_timing_msleep(10);
    }

    WaitForNoKeys();

    lprintf(LO_ALWAYS, "[NUMWORKS] Debug overlay default: %s", s_debug_overlay_visible ? "ON" : "OFF");
    ShowStartupOptionsPrompt();
}

void I_InitScreen_e32()
{
    memset(s_palette, 0, sizeof(s_palette));
    memset(s_palette565, 0, sizeof(s_palette565));
    s_palette_source = NULL;
    memset(s_indexed_frame, 0, sizeof(s_indexed_frame));
    memset(s_rgb565_line, 0, sizeof(s_rgb565_line));
    BuildFullscreenScaleLUTs();
    s_prev_keyboard_state = 0;
    memset(s_debug_lines, 0, sizeof(s_debug_lines));
    s_debug_line_count = 0;
    s_debug_overlay_visible = false;
    s_debug_choice_done = false;
    s_startup_options_done = false;
    s_textured_planes_enabled = true;
    s_visual_extras_enabled = true;

    eadk_display_push_rect_uniform(eadk_screen_rect, eadk_color_black);
    lprintf(LO_ALWAYS, "[NUMWORKS] Screen initialized");
}

void I_CreateBackBuffer_e32()
{
    memset(s_indexed_frame, 0, sizeof(s_indexed_frame));
}

int I_GetVideoWidth_e32()
{
    return SCREENWIDTH;
}

int I_GetVideoHeight_e32()
{
    return SCREENHEIGHT;
}

void I_SetPallete_e32(const byte* pallete)
{
    if (pallete != NULL)
    {
        memcpy(s_palette, pallete, sizeof(s_palette));
        RebuildPalette565(s_palette);
        s_palette_source = pallete;
    }
}

byte* I_GetBackBuffer()
{
    return s_indexed_frame;
}

byte* I_GetFrontBuffer()
{
    return s_indexed_frame;
}

void I_FinishUpdate_e32(const byte* srcBuffer, const byte* pallete, const unsigned int width, const unsigned int height)
{
    (void)width;
    (void)height;

    if (pallete != NULL && pallete != s_palette_source)
    {
        I_SetPallete_e32(pallete);
    }

    const byte* indexed = srcBuffer != NULL ? srcBuffer : s_indexed_frame;

    // eadk_display_wait_for_vblank();

    eadk_rect_t row_rect;
    row_rect.width = EADK_SCREEN_WIDTH;
    row_rect.height = 1;
    row_rect.x = 0;

    for (unsigned int y = 0; y < EADK_SCREEN_HEIGHT; y++)
    {
        const byte* row = &indexed[s_scale_y[y] * kSourceStride];
        row_rect.y = (uint16_t)y;

        for (unsigned int x = 0; x < EADK_SCREEN_WIDTH; x++)
        {
            s_rgb565_line[x] = s_palette565[row[s_scale_x[x]]];
        }

        eadk_display_push_rect(row_rect, s_rgb565_line);
    }

    if (s_debug_overlay_visible)
    {
        ClearOverlayBand();
        DrawZoneOverlay();
        DrawDebugOverlay();
    }
}

void I_ProcessKeyEvents()
{
    eadk_keyboard_state_t current = eadk_keyboard_scan();

    bool combo_now = eadk_keyboard_key_down(current, eadk_key_shift)
        && eadk_keyboard_key_down(current, eadk_key_alpha);
    bool combo_prev = eadk_keyboard_key_down(s_prev_keyboard_state, eadk_key_shift)
        && eadk_keyboard_key_down(s_prev_keyboard_state, eadk_key_alpha);

    if (combo_now && !combo_prev)
    {
        s_debug_overlay_visible = !s_debug_overlay_visible;
        lprintf(LO_ALWAYS, "[NUMWORKS] Debug overlay: %s", s_debug_overlay_visible ? "ON" : "OFF");
    }

    // Consume combo press so gameplay does not receive L/R key events from toggle hotkey.
    if (combo_now)
    {
        s_prev_keyboard_state = current;
        return;
    }

    if (ConsumeCheatHotkeys(current, s_prev_keyboard_state))
    {
        s_prev_keyboard_state = current;
        return;
    }

    ProcessKeyTransition(current, s_prev_keyboard_state, eadk_key_up, KEYD_UP);
    ProcessKeyTransition(current, s_prev_keyboard_state, eadk_key_down, KEYD_DOWN);
    ProcessKeyTransition(current, s_prev_keyboard_state, eadk_key_left, KEYD_LEFT);
    ProcessKeyTransition(current, s_prev_keyboard_state, eadk_key_right, KEYD_RIGHT);

    ProcessKeyTransition(current, s_prev_keyboard_state, eadk_key_exe, KEYD_START);
    ProcessKeyTransition(current, s_prev_keyboard_state, eadk_key_toolbox, KEYD_SELECT);

    ProcessKeyTransition(current, s_prev_keyboard_state, eadk_key_ok, KEYD_A);
    ProcessKeyTransition(current, s_prev_keyboard_state, eadk_key_back, KEYD_B);

    ProcessKeyTransition(current, s_prev_keyboard_state, eadk_key_shift, KEYD_L);
    ProcessKeyTransition(current, s_prev_keyboard_state, eadk_key_alpha, KEYD_R);

    s_prev_keyboard_state = current;
}

int I_GetTime_e32(void)
{
    uint64_t millis = eadk_timing_millis();
    return (int)((millis * TICRATE) / 1000ULL);
}

int I_IsTexturedPlanesEnabled_e32(void)
{
    return s_textured_planes_enabled ? 1 : 0;
}

int I_IsVisualExtrasEnabled_e32(void)
{
    return s_visual_extras_enabled ? 1 : 0;
}

void I_DebugCheckpoint_e32(const char* checkpoint)
{
    ShowDebugChoicePrompt();

    lprintf(LO_ALWAYS, "[CHECKPOINT] %s", checkpoint);
    I_DebugLog_e32(checkpoint);

    if (!s_debug_overlay_visible)
    {
        return;
    }

    eadk_display_push_rect_uniform(eadk_screen_rect, eadk_color_black);
    ClearOverlayBand();
    DrawDebugOverlay();
    DrawZoneOverlay();
    eadk_display_wait_for_vblank();
    WaitForNoKeys();
    WaitForAnyKeyPress();
    WaitForNoKeys();
}

void I_DebugLog_e32(const char* message)
{
    if (message == NULL || message[0] == 0)
    {
        return;
    }

    if (s_debug_line_count < kDebugLineCount)
    {
        s_debug_line_count++;
    }
    else
    {
        for (int i = 1; i < kDebugLineCount; i++)
        {
            memcpy(s_debug_lines[i - 1], s_debug_lines[i], kDebugLineWidth + 1);
        }
    }

    snprintf(s_debug_lines[s_debug_line_count - 1], kDebugLineWidth + 1, "%s", message);
}

void I_DebugPause_e32(const char* reason)
{
    if (reason != NULL)
    {
        lprintf(LO_ALWAYS, "[PAUSE] %s", reason);
        I_DebugLog_e32(reason);
    }

    if (!s_debug_overlay_visible)
    {
        return;
    }

    WaitForNoKeys();
    WaitForAnyKeyPress();
    WaitForNoKeys();
}

#define MAX_MESSAGE_SIZE 1024

void I_Error(const char* error, ...)
{
    char msg[MAX_MESSAGE_SIZE];

    va_list v;
    va_start(v, error);
    vsnprintf(msg, sizeof(msg), error, v);
    va_end(v);

    lprintf(LO_ERROR, "%s", msg);

    eadk_display_push_rect_uniform(eadk_screen_rect, eadk_color_black);
    eadk_display_draw_string("FATAL", (eadk_point_t){4, 4}, true, eadk_color_red, eadk_color_black);
    DrawWrappedText(msg, 4, 28, EADK_SCREEN_WIDTH - 8, EADK_SCREEN_HEIGHT - 32, eadk_color_white, eadk_color_black);

    while (true)
    {
        eadk_timing_msleep(50);
    }
}

void I_Quit_e32()
{
}

#endif
