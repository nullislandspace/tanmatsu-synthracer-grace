// =====================================================================
//  Race the Synth  --  shared UI bridge + primitives (see game_ui.h)
// =====================================================================

#include "game_ui.h"

// The framebuffer bridge. main.c's on_backdrop / on_render set this to
// the engine's live back buffer at the top of each frame.
pax_buf_t* fb = NULL;

float menu_left_x(float w_frac) {
    float const fbw = pax_buf_get_widthf(fb);
    return (fbw - fbw * w_frac) * 0.5f + 28.0f;
}

void draw_left(float x, float y, float h, pax_col_t color, char const* text) {
    rendertext_draw(fb, color, NULL, h, x, y, text);
}

void draw_menu_panel_size(float w_frac, float h_frac) {
    float const fbw = pax_buf_get_widthf(fb);
    float const fbh = pax_buf_get_heightf(fb);
    int   const pw  = (int)(fbw * w_frac);
    int   const ph  = (int)(fbh * h_frac);
    int   const px  = (int)((fbw - (float)pw) * 0.5f);
    int   const py  = (int)((fbh - (float)ph) * 0.5f);
    uint16_t* const pixels = (uint16_t*)pax_buf_get_pixels(fb);
    direct_565_dim_rect(pixels, fb->reverse_endianness, px, py, pw, ph);
}

void draw_chevron(float x, float y, float text_h) {
    draw_left(x, y, text_h, MENU_COL_HILITE, ">");
}
