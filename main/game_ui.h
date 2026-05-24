#pragma once
// =====================================================================
//  Race the Synth  --  shared UI bridge + primitives
// ---------------------------------------------------------------------
//  The framebuffer bridge and the handful of draw primitives every
//  game-side UI module (hud, screens, keybind_ui) shares. main.c's
//  on_backdrop / on_render mirror the engine's live back buffer into the
//  `fb` bridge each frame; everything here draws into it.
// =====================================================================

#include "pax_gfx.h"
#include "se_text.h"        // rendertext_draw
#include "se_direct565.h"   // direct_565_dim_rect

// The current frame's back buffer. The engine (se_run) owns the two
// framebuffers and hands the live one to the draw callbacks; on_backdrop
// and on_render mirror that pointer into this shared `fb` so the game's
// many in-file draw helpers and the PPA submits keep referring to `fb`
// exactly as before the modular split. Defined in game_ui.c; set by the
// callbacks in main.c.
extern pax_buf_t* fb;

// Menu / dialog palette (ARGB8888). The selection highlight is colour
// only (yellow): a selected row never changes size or position, so
// nothing jumps as the cursor moves.
#define MENU_COL_TITLE  0xFFFFFF6Bu   // yellow -- screen titles
#define MENU_COL_HILITE 0xFFFFFF6Bu   // yellow -- selected row
#define MENU_COL_NORMAL 0xFFFFFFFFu   // white  -- unselected rows / body
#define MENU_COL_HINT   0xFFA0A0A8u   // grey   -- footer hints
#define MENU_COL_SUB    0xFF808088u   // dim    -- secondary text

// Hand-laid (non-list) screen geometry. The vertical list menus moved to
// the engine's se_ui renderer; these consts remain for the screens that
// are NOT lists (slot-select, seed entry, stats, credits) which still
// position their text + chevron with them.
#define MENU_TEXT_INSET     28.0f   // panel edge -> chevron gutter
#define MENU_CHEVRON_GUTTER 22.0f   // gutter width reserved for ">"
#define MENU_TOP_PAD        40.0f   // panel top -> title baseline
#define MENU_FOOTER_PAD     32.0f   // footer baseline -> panel bottom

// Left content x for a panel centred at width fraction `w_frac`: the
// panel's left edge plus a fixed text inset.
float menu_left_x(float w_frac);

// Left-aligned vector text -- the primitive every menu element draws
// through (wraps rendertext_draw on the `fb` bridge).
void draw_left(float x, float y, float h, pax_col_t color, char const* text);

// Translucent dim panel behind menu text: a centred rectangle sized by
// the caller (width/height as framebuffer fractions), leaving a synthwave
// border so the menu reads as overlaid on the live scene.
void draw_menu_panel_size(float w_frac, float h_frac);

// Selection chevron, painted into a row's left gutter as a step separate
// from the label so the label never moves.
void draw_chevron(float x, float y, float text_h);
