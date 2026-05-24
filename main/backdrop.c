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

    // Sun cache: sky purple in the gaps + the sun bands at their canonical
    // baseline (top band lands at cache y=0 thanks to dy = +4).
    synthwave_draw_sky(&sun_layer.buf);
    synthwave_draw_sun(&sun_layer.buf, SUN_RENDER_Y_BIAS);

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

bool backdrop_submit_fill_sky(void) {
    // FILL is the per-frame guarantee that no stale obstacle pixel from the
    // previous frame survives in the sky band.
    return se_ppa_fill(fb, 0, SKY_ROWS, SKY_PAX_COL);
}

bool backdrop_submit_sun(int dest_top_log_y) {
    return se_ppa_blit(fb, &sun_layer, dest_top_log_y);
}

bool backdrop_submit_mountains(void) {
    return se_ppa_blend_key(fb, &mountain_layer, MOUNTAIN_DEST_LOG_Y,
                            MOUNTAIN_CK_LO, MOUNTAIN_CK_HI);
}
