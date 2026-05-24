#pragma once
// =====================================================================
//  SynthEngine3D  --  PUBLIC STABLE API  --  UI / list-menu system
// ---------------------------------------------------------------------
//  A data-driven vertical list menu: the game describes a menu as a
//  se_menu_def_t (title, rows, hint, layout), pairs it with live cursor
//  state in a se_menu_t, then per frame feeds it nav actions
//  (se_menu_input) and draws it (se_menu_draw). The engine owns the
//  cursor state machine (move + clamp, activate/back) and the rendering;
//  the game owns what each row *does* and what each row *is* (its data),
//  so the same renderer serves every menu.
//
//  This is the per-frame core: it composes with live content (e.g. a
//  pause overlay drawn over a frozen scene). Theme + geometry come from
//  SE_UI_* in se_config.h (override to reskin). Part of the semver'd
//  public surface (see se_version.h).
//
//  Rendering uses only the engine's own inline leaves (se_text vector
//  text + se_direct565 panel dim) -- no game dependency. A row whose
//  value needs game-specific drawing (e.g. a keybind icon) uses the
//  SE_MENU_VAL_CUSTOM kind + a draw_value callback, keeping that logic
//  on the game side.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>

#include "pax_gfx.h"   // pax_buf_t, pax_col_t

// What a row shows in its value column (to the right of the label).
typedef enum {
    SE_MENU_VAL_NONE = 0,   // plain label row
    SE_MENU_VAL_CHECK,      // label + [X] / [ ]
    SE_MENU_VAL_TEXT,       // label + a free value string
    SE_MENU_VAL_CUSTOM,     // label + game-drawn value (see draw_value)
} se_menu_val_t;

// Draws the value column of a SE_MENU_VAL_CUSTOM row. (x, y) is the value
// column's left edge at the row's text baseline; `h` is the row text
// height; `col` is the row's current colour (highlighted vs normal);
// `ctx` is the row's `ctx` pointer. Lets the game render, e.g., a keybind
// as a key icon while the engine owns the rest of the menu.
typedef void (*se_menu_draw_value_fn)(pax_buf_t* fb, float x, float y,
                                      float h, pax_col_t col, void* ctx);

typedef struct {
    char const*           label;
    se_menu_val_t         kind;
    bool                  checked;     // SE_MENU_VAL_CHECK
    char const*           value;       // SE_MENU_VAL_TEXT
    se_menu_draw_value_fn draw_value;  // SE_MENU_VAL_CUSTOM
    void*                 ctx;         // SE_MENU_VAL_CUSTOM context
} se_menu_row_t;

// A menu's static description. Layout fields left 0 fall back to a
// sensible engine default; panel_w / panel_h are framebuffer-size
// fractions (0 -> a default centred panel).
typedef struct {
    char const*          title;
    char const*          subtitle;   // NULL -> none
    se_menu_row_t const* rows;
    int                  row_count;
    char const*          hint;       // footer hint, NULL -> none
    float                title_h;    // title font height   (0 -> default)
    float                row_h;      // row pitch            (0 -> default)
    float                value_dx;   // value column x offset from label
    float                panel_w;    // panel width fraction (0 -> default)
    float                panel_h;    // panel height fraction(0 -> default)
} se_menu_def_t;

// A live menu: a definition + the current cursor. Zero-initialise, set
// `.def`, and the cursor starts at row 0. A plain value type the game
// owns (coarse, once-per-frame -- no opaque handle needed).
typedef struct {
    se_menu_def_t const* def;
    int                  cursor;
} se_menu_t;

// A nav action fed to se_menu_input(). The game derives these from its
// own input however it likes (RTS maps its consumed menu-nav / confirm /
// cancel onto them).
typedef enum {
    SE_MENU_ACT_NONE = 0,
    SE_MENU_ACT_UP,         // move cursor toward row 0
    SE_MENU_ACT_DOWN,       // move cursor toward the last row
    SE_MENU_ACT_ACTIVATE,   // confirm the current row
    SE_MENU_ACT_BACK,       // cancel / leave the menu
} se_menu_action_t;

// What se_menu_input() reports back.
typedef enum {
    SE_MENU_RESULT_NONE = 0,    // nothing actionable (cursor may have moved)
    SE_MENU_RESULT_ACTIVATED,   // current row activated (read menu->cursor)
    SE_MENU_RESULT_BACK,        // back / cancel requested
} se_menu_result_t;

// Apply one nav action: UP/DOWN move + clamp the cursor (no wrap) and
// return NONE; ACTIVATE/BACK leave the cursor put and return the matching
// result. Call once per action the game detected this frame (e.g. a move
// then an activate); the game then acts on ACTIVATED (using menu->cursor)
// or BACK. A no-op (returns NONE) if `menu` or its def is NULL.
se_menu_result_t se_menu_input(se_menu_t* menu, se_menu_action_t action);

// Draw the menu into `fb`: a dim panel, the title, an optional subtitle,
// the rows (label + value per row kind, chevron on the selected row), and
// an optional footer hint. Pure rendering -- no state change.
void se_menu_draw(se_menu_t const* menu, pax_buf_t* fb);
