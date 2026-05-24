// =====================================================================
//  Race the Synth  --  keybind value rendering (see keybind_ui.h)
// =====================================================================

#include "keybind_ui.h"

#include <stdint.h>
#include <stdio.h>

#include "bsp/input.h"
#include "game_ui.h"   // fb, draw_left
#include "icons.h"
#include "pax_gfx.h"

// Map a scancode to a key icon, or -1 if none exists. icons.c loads
// PNGs for Esc and F1..F6 (see icon_filenames[] in icons.c); those
// keys render as their icon. Every other key falls back to text via
// scancode_name / scancode_glyph. draw_keybind_value also falls back
// to text if the mapped icon failed to load.
static int scancode_icon(uint16_t sc) {
    switch (sc) {
        case BSP_INPUT_SCANCODE_ESC: return ICON_ESC;
        case BSP_INPUT_SCANCODE_F1:  return ICON_F1;
        case BSP_INPUT_SCANCODE_F2:  return ICON_F2;
        case BSP_INPUT_SCANCODE_F3:  return ICON_F3;
        case BSP_INPUT_SCANCODE_F4:  return ICON_F4;
        case BSP_INPUT_SCANCODE_F5:  return ICON_F5;
        case BSP_INPUT_SCANCODE_F6:  return ICON_F6;
        default: return -1;
    }
}

// Word name for a non-printable key (and for the function keys, used
// as the text fallback when an icon is missing). NULL for printable
// keys -- caller falls back to the single glyph.
static char const* scancode_name(uint16_t sc) {
    switch (sc) {
        case BSP_INPUT_SCANCODE_ESC:        return "Esc";
        case BSP_INPUT_SCANCODE_SPACE:      return "Space";
        case BSP_INPUT_SCANCODE_ENTER:      return "Enter";
        case BSP_INPUT_SCANCODE_BACKSPACE:  return "Backspace";
        case BSP_INPUT_SCANCODE_TAB:        return "Tab";
        case BSP_INPUT_SCANCODE_CAPSLOCK:   return "CapsLk";
        case BSP_INPUT_SCANCODE_LEFTSHIFT:  return "L-Shift";
        case BSP_INPUT_SCANCODE_RIGHTSHIFT: return "R-Shift";
        case BSP_INPUT_SCANCODE_LEFTCTRL:   return "Ctrl";
        case BSP_INPUT_SCANCODE_LEFTALT:    return "Alt";
        case BSP_INPUT_SCANCODE_FN:         return "Fn";
        case BSP_INPUT_SCANCODE_F1:  return "F1";
        case BSP_INPUT_SCANCODE_F2:  return "F2";
        case BSP_INPUT_SCANCODE_F3:  return "F3";
        case BSP_INPUT_SCANCODE_F4:  return "F4";
        case BSP_INPUT_SCANCODE_F5:  return "F5";
        case BSP_INPUT_SCANCODE_F6:  return "F6";
        case BSP_INPUT_SCANCODE_F7:  return "F7";
        case BSP_INPUT_SCANCODE_F8:  return "F8";
        case BSP_INPUT_SCANCODE_F9:  return "F9";
        case BSP_INPUT_SCANCODE_F10: return "F10";
        case BSP_INPUT_SCANCODE_F11: return "F11";
        case BSP_INPUT_SCANCODE_F12: return "F12";
        default: return NULL;
    }
}

// Single printable glyph for a scancode, or 0 if the key is not a
// plain printable (caller then uses scancode_name / a hex fallback).
static char scancode_glyph(uint16_t sc) {
    switch (sc) {
        case BSP_INPUT_SCANCODE_1: return '1';
        case BSP_INPUT_SCANCODE_2: return '2';
        case BSP_INPUT_SCANCODE_3: return '3';
        case BSP_INPUT_SCANCODE_4: return '4';
        case BSP_INPUT_SCANCODE_5: return '5';
        case BSP_INPUT_SCANCODE_6: return '6';
        case BSP_INPUT_SCANCODE_7: return '7';
        case BSP_INPUT_SCANCODE_8: return '8';
        case BSP_INPUT_SCANCODE_9: return '9';
        case BSP_INPUT_SCANCODE_0: return '0';
        case BSP_INPUT_SCANCODE_A: return 'A';
        case BSP_INPUT_SCANCODE_B: return 'B';
        case BSP_INPUT_SCANCODE_C: return 'C';
        case BSP_INPUT_SCANCODE_D: return 'D';
        case BSP_INPUT_SCANCODE_E: return 'E';
        case BSP_INPUT_SCANCODE_F: return 'F';
        case BSP_INPUT_SCANCODE_G: return 'G';
        case BSP_INPUT_SCANCODE_H: return 'H';
        case BSP_INPUT_SCANCODE_I: return 'I';
        case BSP_INPUT_SCANCODE_J: return 'J';
        case BSP_INPUT_SCANCODE_K: return 'K';
        case BSP_INPUT_SCANCODE_L: return 'L';
        case BSP_INPUT_SCANCODE_M: return 'M';
        case BSP_INPUT_SCANCODE_N: return 'N';
        case BSP_INPUT_SCANCODE_O: return 'O';
        case BSP_INPUT_SCANCODE_P: return 'P';
        case BSP_INPUT_SCANCODE_Q: return 'Q';
        case BSP_INPUT_SCANCODE_R: return 'R';
        case BSP_INPUT_SCANCODE_S: return 'S';
        case BSP_INPUT_SCANCODE_T: return 'T';
        case BSP_INPUT_SCANCODE_U: return 'U';
        case BSP_INPUT_SCANCODE_V: return 'V';
        case BSP_INPUT_SCANCODE_W: return 'W';
        case BSP_INPUT_SCANCODE_X: return 'X';
        case BSP_INPUT_SCANCODE_Y: return 'Y';
        case BSP_INPUT_SCANCODE_Z: return 'Z';
        case BSP_INPUT_SCANCODE_MINUS:      return '-';
        case BSP_INPUT_SCANCODE_EQUAL:      return '=';
        case BSP_INPUT_SCANCODE_LEFTBRACE:  return '[';
        case BSP_INPUT_SCANCODE_RIGHTBRACE: return ']';
        case BSP_INPUT_SCANCODE_SEMICOLON:  return ';';
        case BSP_INPUT_SCANCODE_APOSTROPHE: return '\'';
        case BSP_INPUT_SCANCODE_GRAVE:      return '`';
        case BSP_INPUT_SCANCODE_BACKSLASH:  return '\\';
        case BSP_INPUT_SCANCODE_COMMA:      return ',';
        case BSP_INPUT_SCANCODE_DOT:        return '.';
        case BSP_INPUT_SCANCODE_SLASH:      return '/';
        default: return 0;
    }
}

// Fill `buf` with the text label for a scancode (used when there is
// no icon for it).
static void keybind_text(uint16_t sc, char* buf, size_t n) {
    char const* name = scancode_name(sc);
    if (name) { snprintf(buf, n, "%s", name); return; }
    char const g = scancode_glyph(sc);
    if (g)    { snprintf(buf, n, "%c", g); return; }
    snprintf(buf, n, "Key 0x%02X", (unsigned)sc);
}

// Draw the value side of a keybind row at (x, y): the function-key
// icon when one exists and loaded, otherwise the text label.
static void draw_keybind_value(float x, float y, float text_h, pax_col_t col, uint16_t sc) {
    int const icon = scancode_icon(sc);
    if (icon >= 0 && icons_width((icon_key_t)icon) > 0) {
        int   const iw = icons_width((icon_key_t)icon);
        int   const ih = icons_height((icon_key_t)icon);
        float const iy = y + text_h * 0.5f - (float)ih * 0.5f;
        // The key-hint PNGs are black glyphs on a transparent
        // background -- invisible on the dim menu panel. Lay down a
        // white tile first so the icon reads like a physical keycap.
        pax_simple_rect(fb, 0xFFFFFFFFu, x, iy, (float)iw, (float)ih);
        icons_blit(fb, (icon_key_t)icon, x, iy);
        return;
    }
    char buf[24];
    keybind_text(sc, buf, sizeof(buf));
    draw_left(x, y, text_h, col, buf);
}

void controls_keybind_draw(pax_buf_t* fb_cb, float x, float y,
                           float h, pax_col_t col, void* ctx) {
    (void)fb_cb;
    draw_keybind_value(x, y, h, col, (uint16_t)(uintptr_t)ctx);
}
