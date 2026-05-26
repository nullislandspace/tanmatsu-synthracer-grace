// =====================================================================
//  Race the Synth  --  in-game HUD readouts + indicators (see hud.h)
// =====================================================================

#include "hud.h"

#include <stdint.h>
#include <stdio.h>

#include "game_ui.h"        // fb, rendertext_*, direct_565_* (via includes)
#include "icons.h"
#include "magicnumbers.h"   // GAME_BOOSTER_FRONT_COLOR, GAME_TRI_FRONT_COLOR
#include "pax_gfx.h"

// Top-right readout stack. Slot 0 = score, slot 1 = stage, slot 2 = v,
// slot 3 = sun. Each line is `text_h + 4` px below the previous.
//
// The v= / sun= / god= readouts are development diagnostics: they
// compile to no-ops in a release build (ENABLE_DEBUGKEYS == 0), so the
// HUD shows only the score/stage above them. Gated here rather than at
// the call sites so the seven callers in play_states.c stay clean.
void draw_speed_readout(float speed_z) {
#if ENABLE_DEBUGKEYS
    char        buf[32];
    snprintf(buf, sizeof(buf), "v=%.1f", speed_z);
    float const text_h = 18.0f;
    pax_vec2f   sz     = rendertext_size(NULL, text_h, buf);
    float const x      = pax_buf_get_widthf(fb) - sz.x - 12.0f;
    rendertext_draw(fb, 0xFFFFFFFF, NULL, text_h, x, 12.0f + 2.0f * (text_h + 4.0f), buf);
#else
    (void)speed_z;
#endif
}

void draw_sun_readout(float sun_y) {
#if ENABLE_DEBUGKEYS
    char        buf[32];
    snprintf(buf, sizeof(buf), "sun=%.1f", sun_y);
    float const text_h = 18.0f;
    pax_vec2f   sz     = rendertext_size(NULL, text_h, buf);
    float const x      = pax_buf_get_widthf(fb) - sz.x - 12.0f;
    rendertext_draw(fb, 0xFFFFFFFF, NULL, text_h, x, 12.0f + 3.0f * (text_h + 4.0f), buf);
#else
    (void)sun_y;
#endif
}

// Debug readout -- slot 4, directly below `sun=`. Shows the godmode
// toggle state (G key) plus the ship's world position so the run
// can be inspected while flown around with crash/stall disabled.
void draw_debug_readout(game_state_t const* g, bool godmode) {
#if ENABLE_DEBUGKEYS
    char        buf[48];
    snprintf(buf, sizeof(buf), "god=%s x=%.2f y=%.2f",
             godmode ? "ON" : "off", g->ship_x_world, g->ship_y);
    float const   text_h = 18.0f;
    pax_col_t const col  = godmode ? 0xFF31FBFBu : 0xFF808080u;
    pax_vec2f     sz     = rendertext_size(NULL, text_h, buf);
    float const   x      = pax_buf_get_widthf(fb) - sz.x - 12.0f;
    rendertext_draw(fb, col, NULL, text_h, x, 12.0f + 4.0f * (text_h + 4.0f), buf);
#else
    (void)g;
    (void)godmode;
#endif
}

// Bottom-left HUD: a solid green upward-pointing triangle that's
// visible whenever a boost is active (any phase that isn't IDLE).
// Sized at 3x the debug-readout text height so it's easy to read
// out of the corner of the eye while flying. Drawn via the direct
// 565 rasterizer -- single triangle, ~zero cost.
void draw_boost_indicator(game_state_t const* g) {
    if (g->boost_phase == BOOST_IDLE) return;
    float const text_h    = 18.0f;
    float const h         = text_h * 3.0f;   // 54 px
    float const w         = h * 0.866f;      // equilateral-ish footprint
    float const margin    = 12.0f;
    float const fb_h      = pax_buf_get_heightf(fb);
    float const apex_x    = margin + w * 0.5f;
    float const apex_y    = fb_h - margin - h;
    float const bl_x      = margin;
    float const br_x      = margin + w;
    float const base_y    = fb_h - margin;

    uint16_t* const fb_pixels = (uint16_t*)pax_buf_get_pixels(fb);
    uint16_t  const packed    = direct_565_pack(GAME_BOOSTER_FRONT_COLOR, fb->reverse_endianness);
    direct_565_tri(fb_pixels, apex_x, apex_y, bl_x, base_y, br_x, base_y, packed);
}

// Jump-charge inventory HUD (Phase 9.1f) -- one red diamond per
// stored jump charge, anchored to the bottom-right corner and
// growing leftward. Nothing is drawn at zero charges.
#define JUMP_HUD_COLOR   0xFFFF4848u
void draw_jump_inventory(game_state_t const* g) {
    if (g->jump_charges <= 0) return;
    // Diamonds match the boost indicator's size (its `text_h * 3`,
    // = 54 px point-to-point), so the two HUD symbols read as a set.
    float const margin  = 12.0f;
    float const r       = (18.0f * 3.0f) * 0.5f;   // 27 px half-extent
    float const spacing = 2.0f * r + 6.0f;
    float const fb_w    = pax_buf_get_widthf(fb);
    float const fb_h    = pax_buf_get_heightf(fb);
    float const cy      = fb_h - margin - r;
    for (int i = 0; i < g->jump_charges; i++) {
        // Right-anchored: charge 0 sits in the corner, the rest
        // extend left. A square rotated 45 deg -- two triangles split
        // on the vertical diagonal.
        float const cx = fb_w - margin - r - (float)i * spacing;
        pax_simple_tri(fb, JUMP_HUD_COLOR, cx, cy - r, cx + r, cy, cx, cy + r);
        pax_simple_tri(fb, JUMP_HUD_COLOR, cx, cy - r, cx, cy + r, cx - r, cy);
    }
}

// Bottom-right shield-charge readout (Phase 9.2): one violet hexagon
// per banked shield charge, in a row directly above the jump-charge
// diamonds. Same symbol size so the two HUD inventories read as a
// set. Nothing is drawn at zero charges.
#define SHIELD_HUD_COLOR  0xFFB060FFu
void draw_shield_inventory(game_state_t const* g) {
    if (g->shield_charges <= 0) return;
    float const margin  = 12.0f;
    float const r       = (18.0f * 3.0f) * 0.5f;   // 27 px -- matches the diamonds
    float const spacing = 2.0f * r + 6.0f;
    float const fb_w    = pax_buf_get_widthf(fb);
    float const fb_h    = pax_buf_get_heightf(fb);
    float const cy      = fb_h - margin - r - spacing;   // one row up from jumps
    // Pointy-top unit hexagon corners (corner k at 90 + 60*k degrees).
    static float const HX[6] = { 0.0f, -0.866025f, -0.866025f, 0.0f, 0.866025f, 0.866025f };
    static float const HY[6] = { 1.0f,  0.5f,      -0.5f,     -1.0f, -0.5f,      0.5f      };
    for (int i = 0; i < g->shield_charges; i++) {
        float const cx = fb_w - margin - r - (float)i * spacing;
        // Filled hexagon -- a 6-triangle fan from the centre.
        for (int k = 0; k < 6; k++) {
            int const j = (k + 1) % 6;
            pax_simple_tri(fb, SHIELD_HUD_COLOR,
                           cx, cy,
                           cx + HX[k] * r, cy - HY[k] * r,
                           cx + HX[j] * r, cy - HY[j] * r);
        }
    }
}

// Bottom-right checkpoint readout (Phase 9.3): a single black/white
// 3x3 checkerboard square -- the held checkpoint -- two rows above the
// jump-charge diamonds. The player holds at most one. Nothing is
// drawn when no checkpoint is held.
void draw_checkpoint_inventory(game_state_t const* g) {
    if (!g->checkpoint_held) return;
    float const margin  = 12.0f;
    float const r       = (18.0f * 3.0f) * 0.5f;   // 27 px -- matches the other symbols
    float const spacing = 2.0f * r + 6.0f;
    float const fb_w    = pax_buf_get_widthf(fb);
    float const fb_h    = pax_buf_get_heightf(fb);
    float const cx      = fb_w - margin - r;                  // corner column
    float const cy      = fb_h - margin - r - 2.0f * spacing; // two rows up
    float const cell    = (2.0f * r) / 3.0f;
    float const x0      = cx - r;
    float const y0      = cy - r;
    for (int gy = 0; gy < 3; gy++) {
        for (int gx = 0; gx < 3; gx++) {
            uint32_t const col = ((gx + gy) & 1) ? 0xFFF0F0F0u : 0xFF101010u;
            pax_simple_rect(fb, col, x0 + (float)gx * cell, y0 + (float)gy * cell,
                            cell, cell);
        }
    }
}

// Phase 6 multiplier-HUD layout. Constants are kept beside the draws
// that use them; the F4 hint y baseline (HUD_HINT_Y_BASE) sits below
// the panel and is referenced by draw_pause_hint.
#define MUL_PANEL_X        12
#define MUL_PANEL_Y        12
#define MUL_PANEL_W        128
#define MUL_PANEL_H        72
#define MUL_TRI_COUNT      4
#define MUL_TRI_W          22
#define MUL_TRI_H          18
#define MUL_TRI_GAP        4
#define MUL_TRI_ROW_Y      (MUL_PANEL_Y + 10)
#define MUL_TEXT_Y         (MUL_PANEL_Y + MUL_PANEL_H - 30)
#define MUL_PANEL_BG       0xFF181828u
#define MUL_TRI_LIT_RGB    GAME_TRI_FRONT_COLOR
#define MUL_TRI_DARK_RGB   0xFF333344u
#define HUD_HINT_Y_BASE    ((float)(MUL_PANEL_Y + MUL_PANEL_H + 12))

// F4-to-pause hint, drawn during PLAYING below the multiplier HUD
// panel. (The dev-only "F1 to exit" hint that used to sit above it
// has been removed -- F1 still exits, it just isn't advertised.)
void draw_pause_hint(void) {
    char const* prompt   = "to pause";
    float const prompt_h = 18.0f;
    int         icon_w   = icons_width(ICON_F4);
    float const x_margin = 12.0f;
    float const y        = HUD_HINT_Y_BASE;
    if (icon_w > 0) {
        float const gap    = 8.0f;
        int         icon_h = icons_height(ICON_F4);
        float       icon_y = y + prompt_h / 2.0f - (float)icon_h / 2.0f;
        float       text_x = x_margin + (float)icon_w + gap;
        icons_blit(fb, ICON_F4, x_margin, icon_y);
        rendertext_draw(fb, 0xFFFFFFFF, NULL, prompt_h, text_x, y, prompt);
    } else {
        char const* fallback = "F4 to pause";
        rendertext_draw(fb, 0xFFFFFFFF, NULL, prompt_h, x_margin, y, fallback);
    }
}

// Top-right HUD during PLAYING and GAME_OVER: score + multiplier.
void draw_score_readout(game_state_t const* g) {
    char        buf[48];
    float const text_h = 18.0f;
    float const fbw    = pax_buf_get_widthf(fb);

    snprintf(buf, sizeof(buf), "score=%lld", (long long)g->score);
    pax_vec2f sz = rendertext_size(NULL, text_h, buf);
    rendertext_draw(fb, 0xFFFFFF6Bu, NULL, text_h,
                    fbw - sz.x - 12.0f, 12.0f, buf);
}

// Top-left HUD: multiplier panel (Phase 6). Opaque dark-grey
// rectangle holding two rows:
//   1. Four small triangles, filled blue for collected and dark
//      grey for empty. Lit count = `pickups_tri % 5` (always
//      0..4 -- the 5th Tri ticks the multiplier and resets the
//      row, so the 5th slot never visually holds).
//   2. The current multiplier as `xN`.
void draw_multiplier_panel(game_state_t const* g) {
    // Opaque background fill -- uses pax_simple_rect (not direct_565
    // dim) so the panel is fully readable regardless of scenery.
    pax_simple_rect(fb, MUL_PANEL_BG,
                    (float)MUL_PANEL_X, (float)MUL_PANEL_Y,
                    (float)MUL_PANEL_W, (float)MUL_PANEL_H);

    // 4-slot Tri progress row. `pickups_tri` is monotonic per run;
    // `pickups_tri % 5` is the count of slots to light up. The 5th
    // slot is never visually held -- picking up the 5th Tri ticks
    // the multiplier and resets the row in the same instant.
    int const lit_count = (int)(g->pickups_tri % 5);
    int const total_row_w = MUL_TRI_COUNT * MUL_TRI_W + (MUL_TRI_COUNT - 1) * MUL_TRI_GAP;
    int const row_x0    = MUL_PANEL_X + (MUL_PANEL_W - total_row_w) / 2;

    for (int i = 0; i < MUL_TRI_COUNT; i++) {
        int const tx     = row_x0 + i * (MUL_TRI_W + MUL_TRI_GAP);
        pax_col_t const col = (i < lit_count) ? MUL_TRI_LIT_RGB : MUL_TRI_DARK_RGB;
        // Upward-pointing triangle: apex top-centre, base on the
        // bottom edge. Same shape as the in-world Tri pyramid from
        // the player's viewpoint (apex up).
        pax_simple_tri(fb, col,
                       (float)(tx + MUL_TRI_W / 2), (float)MUL_TRI_ROW_Y,
                       (float)tx,                   (float)(MUL_TRI_ROW_Y + MUL_TRI_H),
                       (float)(tx + MUL_TRI_W),     (float)(MUL_TRI_ROW_Y + MUL_TRI_H));
    }

    // Multiplier text on the bottom row. "xN" centred horizontally
    // within the panel.
    char buf[16];
    snprintf(buf, sizeof(buf), "x%d", g->multiplier);
    float const text_h = 24.0f;
    pax_vec2f   sz     = rendertext_size(NULL, text_h, buf);
    float const tx     = (float)MUL_PANEL_X + ((float)MUL_PANEL_W - sz.x) * 0.5f;
    rendertext_draw(fb, 0xFFFFFF6Bu, NULL, text_h, tx, (float)MUL_TEXT_Y, buf);
}

// Top-right HUD slot 1 -- stage number. Same upcoming-stage rule as
// the rest-area banner: during a rest area between stages N and
// N+1, w->stage is still N, so we add 1 so the HUD shows the same
// number as the banner above. Green to match the banner.
void draw_stage_readout(world_state_t const* w) {
    int const stage = (int)w->stage + (w->area.type == AREA_TYPE_REST ? 1 : 0);
    char      buf[32];
    snprintf(buf, sizeof(buf), "Stage: %d", stage);
    float const text_h = 18.0f;
    pax_vec2f   sz     = rendertext_size(NULL, text_h, buf);
    float const x      = pax_buf_get_widthf(fb) - sz.x - 12.0f;
    rendertext_draw(fb, GAME_BOOSTER_FRONT_COLOR, NULL, text_h,
                    x, 12.0f + (text_h + 4.0f), buf);
}

// "Stage: N" banner shown during rest areas (between stages and at
// run start). Translucent dark panel sized to fit the text, centred
// horizontally near the top of the screen.
void draw_stage_banner(int stage) {
    char buf[32];
    snprintf(buf, sizeof(buf), "Stage: %d", stage);

    float const fbw    = pax_buf_get_widthf(fb);
    float const fbh    = pax_buf_get_heightf(fb);
    float const text_h = 48.0f;
    pax_vec2f   sz     = rendertext_size(NULL, text_h, buf);

    int const pad_x = 32;
    int const pad_y = 14;
    int const pw    = (int)sz.x + 2 * pad_x;
    int const ph    = (int)text_h + 2 * pad_y;
    int const px    = (int)((fbw - (float)pw) * 0.5f);
    int const py    = (int)(fbh * 0.04f);

    uint16_t* const pixels = (uint16_t*)pax_buf_get_pixels(fb);
    direct_565_dim_rect(pixels, fb->reverse_endianness, px, py, pw, ph);

    float const tx = (fbw - sz.x) * 0.5f;
    float const ty = (float)py + (float)pad_y;
    rendertext_draw(fb, GAME_BOOSTER_FRONT_COLOR, NULL, text_h, tx, ty, buf);
}

// The "Stage: N" banner is shown only during the *tail* of a rest
// area -- the last STAGE_BANNER_LEAD_Z world-z (~5 s at cruise)
// before the rest ends and the next stage proper begins.
#define STAGE_BANNER_LEAD_Z  100.0f

bool stage_banner_visible(world_state_t const* w) {
    return w->area.type == AREA_TYPE_REST
        && w->area.length_remaining_z <= STAGE_BANNER_LEAD_Z;
}
