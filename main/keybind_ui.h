#pragma once
// =====================================================================
//  Race the Synth  --  keybind value rendering
// ---------------------------------------------------------------------
//  Renders a bound key (scancode) as a key-cap icon (Esc / F1..F6 PNGs
//  from icons.c) or a text label. The only public entry is the se_ui
//  SE_MENU_VAL_CUSTOM value drawer for the Controls menu's keybind rows;
//  the scancode->icon/name/glyph mapping is internal.
// =====================================================================

#include "pax_gfx.h"   // pax_buf_t, pax_col_t

// se_ui SE_MENU_VAL_CUSTOM value drawer for the Controls keybind rows:
// renders the bound key as an icon/label. `ctx` carries the scancode
// packed as the pointer value. Draws on the game_ui `fb` bridge (which
// during on_render points at the same back buffer the engine passes in
// `fb_cb`).
void controls_keybind_draw(pax_buf_t* fb_cb, float x, float y,
                           float h, pax_col_t col, void* ctx);
