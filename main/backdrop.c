// =====================================================================
//  Race the Synth  --  synthwave backdrop (PPA compositor) (see backdrop.h)
//
//  Thin synthwave-specific wrapper over the engine PPA helper (se_ppa.h):
//  this file owns the artwork + the band recipe; se_ppa owns the clients,
//  the async completion latch, the orientation maths and the PSRAM
//  allocation. The submit order + waits are main.c's on_backdrop.
// =====================================================================

#include "backdrop.h"

#include "game_ui.h"        // fb (framebuffer bridge)
#include "magicnumbers.h"   // SKY_*, SUN_*, MOUNTAIN_*
#include "pax_gfx.h"
#include "se_ppa.h"
#include "se_run.h"         // se_display_info
#include "synthwave.h"

// Mountain colour-key window on the PPA-expanded RGB888 foreground value.
// Pure green never appears in the synthwave palette, so a tight band around
// it keys the silhouette's background out without false-matching artwork.
// 565->888 expansion is "shift" (g=0x3F -> 0xFC) on some silicon and
// "replicate" (-> 0xFF) on others, so the window covers both. (The cache is
// painted with MOUNTAIN_KEY_PAX_COL = 0xFF00FF00.)
#define MOUNTAIN_CK_LO 0x0000FC00u
#define MOUNTAIN_CK_HI 0x0000FF00u

// Pre-rendered backdrop layers (engine-allocated PSRAM caches). The sun and
// mountains live in separate caches so the sun can slide vertically
// (SRM dest offset) while the mountains stay put.
static se_ppa_layer_t sun_layer      = {0};
static se_ppa_layer_t mountain_layer = {0};

void backdrop_init(void) {
    if (!se_ppa_init()) {
        return;  // logged by se_ppa; submits below become no-ops
    }

    // Match the engine framebuffers' format / endianness / orientation so
    // the layer caches share their raw layout (the PPA reads them in the
    // same orientation as it writes the fb).
    se_display_info_t di;
    se_display_info(&di);

    if (!se_ppa_layer_alloc(&sun_layer, SUN_CACHE_LOG_W, SUN_CACHE_LOG_H,
                            di.pax_format, di.reversed, di.orientation) ||
        !se_ppa_layer_alloc(&mountain_layer, MOUNTAIN_CACHE_LOG_W, MOUNTAIN_CACHE_LOG_H,
                            di.pax_format, di.reversed, di.orientation)) {
        return;  // logged by se_ppa; submits become no-ops
    }

    // Sun cache: sky purple in the gaps + the sun bands. The cache is only
    // as wide as the sun's bounding box, so draw the sun shifted left by
    // SUN_CACHE_LOG_X (the cache's screen-x origin); the blit puts it back at
    // the right x. dy = +4 lands the top band at cache y=0.
    synthwave_draw_sky(&sun_layer.buf);
    synthwave_draw_sun(&sun_layer.buf, -(float)SUN_CACHE_LOG_X, SUN_RENDER_Y_BIAS);

    // Mountain cache: green colour-key background + silhouette + wireframes
    // + horizon line, shifted up so the top of the visible band sits at y=0.
    pax_background(&mountain_layer.buf, MOUNTAIN_KEY_PAX_COL);
    synthwave_draw_mountains(&mountain_layer.buf, MOUNTAIN_RENDER_Y_BIAS);
    synthwave_draw_wireframe(&mountain_layer.buf, MOUNTAIN_RENDER_Y_BIAS);
    synthwave_draw_top_grid(&mountain_layer.buf, MOUNTAIN_RENDER_Y_BIAS);

    // Flush both caches to PSRAM so the PPA's DMA reads finished pixels.
    // One-shot -- neither cache changes after this.
    se_ppa_layer_flush(&sun_layer);
    se_ppa_layer_flush(&mountain_layer);
}

bool backdrop_submit_fill_sky(uint32_t job_id) {
    // FILL is the per-frame guarantee that no stale obstacle pixel from the
    // previous frame survives in the sky band.
    return se_ppa_fill(fb, job_id, 0, SKY_ROWS, SKY_PAX_COL);
}

bool backdrop_submit_fill_floor(uint32_t job_id, bool fully_shadowed) {
    // Floor base: solid fill of the below-horizon region, replacing the old
    // CPU pax_simple_rect. Same FILL op type as the sky, enqueued right after
    // it so the pump runs them back to back while the CPU geometry-prepare
    // overlaps both. The grid lines and shadow quads are drawn on top by the
    // CPU after this completes. Once the sun has fully set the world is
    // uniformly in shadow, so the base itself takes the shadow colour.
    uint32_t const col = fully_shadowed ? GAME_SHADOW_FLOOR_COLOR : FLOOR_BASE_PAX_COL;
    return se_ppa_fill(fb, job_id, FLOOR_FILL_TOP, FLOOR_FILL_ROWS, col);
}

bool backdrop_submit_sun(uint32_t job_id, int dest_top_log_y) {
    // Sprite-blit only the sun's bounding box (SUN_CACHE_LOG_W wide) to its
    // screen x (SUN_CACHE_LOG_X), and CLIP the bottom at the horizon: the
    // cache is SUN_CACHE_LOG_H tall and the dest sinks with the sun, so
    // without clipping the lower rows would spill past SKY_ROWS into the
    // floor. h<=0 (sun fully below horizon) is a no-op inside se_ppa.
    int const max_h = SKY_ROWS - dest_top_log_y;   // rows that stay above the horizon
    int const h     = (SUN_CACHE_LOG_H < max_h) ? SUN_CACHE_LOG_H : max_h;
    return se_ppa_blit_rect(fb, job_id, &sun_layer, 0, 0, SUN_CACHE_LOG_W, h,
                            SUN_CACHE_LOG_X, dest_top_log_y);
}

bool backdrop_submit_mountains(uint32_t job_id) {
    return se_ppa_blend_key(fb, job_id, &mountain_layer, MOUNTAIN_DEST_LOG_Y,
                            MOUNTAIN_CK_LO, MOUNTAIN_CK_HI);
}
