// =====================================================================
//  Race the Synth  --  synthwave backdrop (PPA compositor) (see backdrop.h)
// =====================================================================

#include "backdrop.h"

#include <stdint.h>

#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "game_ui.h"        // fb (framebuffer bridge)
#include "magicnumbers.h"   // SKY_*, SUN_*, MOUNTAIN_*, PPA_PSRAM_CACHE_LINE
#include "pax_gfx.h"
#include "synthwave.h"

static char const TAG[] = "backdrop";

// Raw framebuffer geometry, captured at backdrop_init from the engine's
// resolved display. The PPA band math needs the raw dimensions.
static size_t s_h_res = 0;
static size_t s_v_res = 0;

// PPA clients -- one per operation type because the driver ties a
// client handle to a single op. All three feed a shared counting
// semaphore via their `on_trans_done` callbacks: every frame we
// submit FILL+SRM+BLEND non-blocking, do CPU floor work, then take
// the semaphore three times before any foreground rendering touches
// the sky region. PPA is a single hardware engine, so the ops
// execute sequentially in submission order even though we kick
// them off in one burst.
static ppa_client_handle_t ppa_srm_client   = NULL;
static ppa_client_handle_t ppa_blend_client = NULL;
static ppa_client_handle_t ppa_fill_client  = NULL;
static SemaphoreHandle_t   ppa_done_sem     = NULL;
static int                 ppa_pending_n    = 0;

// Pre-rendered backdrop layers, split into two PPA-driven caches so
// the sun can move independently of the mountains. The per-frame
// pipeline is:
//   1) PPA FILL  -- sky purple across the whole above-horizon region.
//   2) PPA SRM   -- copy `sun_cache` into fb at the sun's current
//      vertical offset.
//   3) PPA BLEND -- composite `mountain_cache` over fb with a green
//      colour-key so the sky/sun shows through outside the silhouette.
static pax_buf_t sun_cache       = {0};
static void*     sun_pixels      = NULL;
static size_t    sun_size        = 0;
static pax_buf_t mountain_cache  = {0};
static void*     mountain_pixels = NULL;
static size_t    mountain_size   = 0;

// PPA "transaction done" callback. Runs in interrupt context; gives
// the shared counting semaphore once per completed op so the main
// loop can proceed past `bgwait` after taking it N times. Return
// value tells the PPA driver whether a higher-priority task became
// ready (yes if xSemaphoreGiveFromISR woke one).
static bool ppa_on_trans_done(ppa_client_handle_t client, ppa_event_data_t *event_data, void *user_data) {
    (void)client;
    (void)event_data;
    (void)user_data;
    BaseType_t hpw = pdFALSE;
    xSemaphoreGiveFromISR(ppa_done_sem, &hpw);
    return hpw == pdTRUE;
}

// PPA picture-block axis layout under PAX_O_ROT_CW. PPA operates on
// raw memory; PAX rotates logical -> raw by `rx = raw_w - 1 - ly`,
// `ry = lx`. So a logical rectangle of (lx0..lx1, ly0..ly1) becomes
// raw block (rx_start = raw_w - 1 - ly1, ry_start = lx0) with
// block_w = ly1 - ly0 + 1 (height in logical -> width in raw) and
// block_h = lx1 - lx0 + 1 (width in logical -> height in raw).
//
// Both fb and the layer caches were created with the same orientation
// + raw layout, so the rotation transform is identical on both sides.
typedef struct {
    uint32_t pic_w;
    uint32_t pic_h;
    uint32_t block_w;
    uint32_t block_h;
    uint32_t block_offset_x;
    uint32_t block_offset_y;
} ppa_raw_blk_t;

// Convert a full-width logical row band [ly_top, ly_top + log_h)
// into the corresponding raw block for a buffer of size raw_w x raw_h
// with PAX_O_ROT_CW. The logical x range is always [0, 800), full
// width.
static ppa_raw_blk_t ppa_band_to_raw(uint32_t raw_w, uint32_t raw_h, int log_y_top, int log_h) {
    ppa_raw_blk_t b;
    b.pic_w          = raw_w;
    b.pic_h          = raw_h;
    b.block_w        = (uint32_t)log_h;          // logical-h -> raw-w
    b.block_h        = raw_h;                    // logical x range [0, 800) -> full raw_h
    b.block_offset_x = (uint32_t)((int)raw_w - log_y_top - log_h);
    b.block_offset_y = 0;
    return b;
}

bool backdrop_submit_fill_sky(void) {
    ppa_raw_blk_t const dst = ppa_band_to_raw((uint32_t)s_h_res, (uint32_t)s_v_res, 0, SKY_ROWS);
    ppa_fill_oper_config_t cfg = {
        .out = {
            .buffer         = (void*)pax_buf_get_pixels(fb),
            .buffer_size    = pax_buf_get_size(fb),
            .pic_w          = dst.pic_w,
            .pic_h          = dst.pic_h,
            .block_offset_x = dst.block_offset_x,
            .block_offset_y = dst.block_offset_y,
            .fill_cm        = PPA_FILL_COLOR_MODE_RGB565,
        },
        .fill_block_w   = dst.block_w,
        .fill_block_h   = dst.block_h,
        .fill_argb_color = {.a = 0xFF, .r = (SKY_PAX_COL >> 16) & 0xFF,
                            .g = (SKY_PAX_COL >>  8) & 0xFF,
                            .b =  SKY_PAX_COL        & 0xFF},
        .mode      = PPA_TRANS_MODE_NON_BLOCKING,
        .user_data = NULL,
    };
    esp_err_t err = ppa_do_fill(ppa_fill_client, &cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ppa_do_fill failed: %d", err);
        return false;
    }
    ppa_pending_n++;
    return true;
}

bool backdrop_submit_sun(int dest_top_log_y) {
    uint32_t const fb_raw_w   = (uint32_t)s_h_res;
    uint32_t const fb_raw_h   = (uint32_t)s_v_res;
    uint32_t const sun_raw_w  = SUN_CACHE_LOG_H;   // ROT_CW transpose
    uint32_t const sun_raw_h  = SUN_CACHE_LOG_W;

    ppa_raw_blk_t const src = ppa_band_to_raw(sun_raw_w, sun_raw_h, 0, SUN_CACHE_LOG_H);
    ppa_raw_blk_t const dst = ppa_band_to_raw(fb_raw_w, fb_raw_h, dest_top_log_y, SUN_CACHE_LOG_H);

    ppa_srm_oper_config_t cfg = {
        .in = {
            .buffer         = sun_pixels,
            .pic_w          = src.pic_w,
            .pic_h          = src.pic_h,
            .block_w        = src.block_w,
            .block_h        = src.block_h,
            .block_offset_x = src.block_offset_x,
            .block_offset_y = src.block_offset_y,
            .srm_cm         = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer         = (void*)pax_buf_get_pixels(fb),
            .buffer_size    = pax_buf_get_size(fb),
            .pic_w          = dst.pic_w,
            .pic_h          = dst.pic_h,
            .block_offset_x = dst.block_offset_x,
            .block_offset_y = dst.block_offset_y,
            .srm_cm         = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle    = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x           = 1.0f,
        .scale_y           = 1.0f,
        .mirror_x          = false,
        .mirror_y          = false,
        .rgb_swap          = false,
        .byte_swap         = false,
        .alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .mode              = PPA_TRANS_MODE_NON_BLOCKING,
        .user_data         = NULL,
    };
    esp_err_t err = ppa_do_scale_rotate_mirror(ppa_srm_client, &cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ppa_do_scale_rotate_mirror(sun) failed: %d", err);
        return false;
    }
    ppa_pending_n++;
    return true;
}

bool backdrop_submit_mountains(void) {
    uint32_t const fb_raw_w   = (uint32_t)s_h_res;
    uint32_t const fb_raw_h   = (uint32_t)s_v_res;
    uint32_t const m_raw_w    = MOUNTAIN_CACHE_LOG_H;
    uint32_t const m_raw_h    = MOUNTAIN_CACHE_LOG_W;

    ppa_raw_blk_t const fg  = ppa_band_to_raw(m_raw_w, m_raw_h, 0, MOUNTAIN_CACHE_LOG_H);
    ppa_raw_blk_t const dst = ppa_band_to_raw(fb_raw_w, fb_raw_h, MOUNTAIN_DEST_LOG_Y, MOUNTAIN_CACHE_LOG_H);

    ppa_blend_oper_config_t cfg = {
        .in_bg = {
            .buffer         = (void*)pax_buf_get_pixels(fb),
            .pic_w          = dst.pic_w,
            .pic_h          = dst.pic_h,
            .block_w        = dst.block_w,
            .block_h        = dst.block_h,
            .block_offset_x = dst.block_offset_x,
            .block_offset_y = dst.block_offset_y,
            .blend_cm       = PPA_BLEND_COLOR_MODE_RGB565,
        },
        .in_fg = {
            .buffer         = mountain_pixels,
            .pic_w          = fg.pic_w,
            .pic_h          = fg.pic_h,
            .block_w        = fg.block_w,
            .block_h        = fg.block_h,
            .block_offset_x = fg.block_offset_x,
            .block_offset_y = fg.block_offset_y,
            .blend_cm       = PPA_BLEND_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer         = (void*)pax_buf_get_pixels(fb),
            .buffer_size    = pax_buf_get_size(fb),
            .pic_w          = dst.pic_w,
            .pic_h          = dst.pic_h,
            .block_offset_x = dst.block_offset_x,
            .block_offset_y = dst.block_offset_y,
            .blend_cm       = PPA_BLEND_COLOR_MODE_RGB565,
        },
        .bg_rgb_swap          = false,
        .bg_byte_swap         = false,
        .bg_alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .fg_rgb_swap          = false,
        .fg_byte_swap         = false,
        .fg_alpha_update_mode = PPA_ALPHA_NO_CHANGE,

        // Foreground colour-key: when a foreground pixel's expanded
        // RGB888 value falls in [low, high], that pixel is treated as
        // transparent -- the background passes through. The window
        // covers both possible 565->888 expansion modes (shift gives
        // g=0xFC, replicate gives g=0xFF) so we don't have to know
        // which the silicon uses.
        .fg_ck_en             = true,
        .fg_ck_rgb_low_thres  = {.r = 0x00, .g = 0xFC, .b = 0x00},
        .fg_ck_rgb_high_thres = {.r = 0x00, .g = 0xFF, .b = 0x00},
        .bg_ck_en             = false,
        .ck_rgb_default_val   = {.r = 0, .g = 0, .b = 0},
        .ck_reverse_bg2fg     = false,

        .mode      = PPA_TRANS_MODE_NON_BLOCKING,
        .user_data = NULL,
    };
    esp_err_t err = ppa_do_blend(ppa_blend_client, &cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ppa_do_blend(mountains) failed: %d", err);
        return false;
    }
    ppa_pending_n++;
    return true;
}

void backdrop_wait_one(void) {
    if (ppa_pending_n <= 0) return;
    if (xSemaphoreTake(ppa_done_sem, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "PPA wait_one timed out (%d pending)", ppa_pending_n);
        return;
    }
    ppa_pending_n--;
}

void backdrop_init(size_t h_res, size_t v_res,
                   pax_buf_type_t format, bool reversed,
                   pax_orientation_t orientation) {
    s_h_res = h_res;
    s_v_res = v_res;

    // Layer caches: tight bounding boxes for the sun bands and the
    // visible mountain band. Both live in PSRAM, both are aligned to
    // the PPA cache-line requirement (128 B on ESP32-P4 external mem).
    // Allocation size is rounded *up* to the cache line so the
    // tail of the buffer doesn't share a line with neighbouring
    // allocations -- picture dimensions stay tight regardless.
    size_t const aligned_sun_size      = (((size_t)SUN_CACHE_LOG_W      * SUN_CACHE_LOG_H      * 2u)
                                          + PPA_PSRAM_CACHE_LINE - 1) & ~(size_t)(PPA_PSRAM_CACHE_LINE - 1);
    size_t const aligned_mountain_size = (((size_t)MOUNTAIN_CACHE_LOG_W * MOUNTAIN_CACHE_LOG_H * 2u)
                                          + PPA_PSRAM_CACHE_LINE - 1) & ~(size_t)(PPA_PSRAM_CACHE_LINE - 1);
    sun_size        = aligned_sun_size;
    mountain_size   = aligned_mountain_size;
    sun_pixels      = heap_caps_aligned_alloc(PPA_PSRAM_CACHE_LINE, sun_size,      MALLOC_CAP_SPIRAM);
    mountain_pixels = heap_caps_aligned_alloc(PPA_PSRAM_CACHE_LINE, mountain_size, MALLOC_CAP_SPIRAM);
    if (sun_pixels == NULL || mountain_pixels == NULL) {
        ESP_LOGE(TAG, "Failed to allocate layer caches (sun=%p mount=%p)", sun_pixels, mountain_pixels);
        return;
    }
    // pax_buf_init takes *raw* dimensions; the logical shape comes
    // out via pax_buf_set_orientation. Under PAX_O_ROT_CW the raw
    // layout is the logical layout transposed, so a logical
    // 800-wide x 180-tall cache needs a 180-wide x 800-tall raw
    // buffer. Passing the logical dims here would give PAX an
    // 800-wide x 180-tall raw buffer, then ROT_CW would swap them
    // back to 180 logical wide x 800 logical tall -- every sun-band
    // x coordinate (294..506) would then clip out of bounds.
    pax_buf_init(&sun_cache, sun_pixels, SUN_CACHE_LOG_H, SUN_CACHE_LOG_W, format);
    pax_buf_reversed(&sun_cache, reversed);
    pax_buf_set_orientation(&sun_cache, orientation);

    pax_buf_init(&mountain_cache, mountain_pixels, MOUNTAIN_CACHE_LOG_H, MOUNTAIN_CACHE_LOG_W, format);
    pax_buf_reversed(&mountain_cache, reversed);
    pax_buf_set_orientation(&mountain_cache, orientation);

    // Sun cache: sky purple in the gaps + sun bands at their canonical
    // baseline (top band lands at cache y=0 thanks to dy = +4).
    synthwave_draw_sky(&sun_cache);
    synthwave_draw_sun(&sun_cache, SUN_RENDER_Y_BIAS);

    // Mountain cache: green colour-key background + mountain silhouette
    // + wireframes + horizon line, all shifted up by 94 so the top of
    // the visible mountain band sits at cache y=0.
    pax_background(&mountain_cache, MOUNTAIN_KEY_PAX_COL);
    synthwave_draw_mountains(&mountain_cache, MOUNTAIN_RENDER_Y_BIAS);
    synthwave_draw_wireframe(&mountain_cache, MOUNTAIN_RENDER_Y_BIAS);
    synthwave_draw_top_grid(&mountain_cache, MOUNTAIN_RENDER_Y_BIAS);

    // Flush both caches to PSRAM so PPA's DMA reads see the finished
    // pixels. One-shot at boot -- neither cache changes after this.
    esp_err_t cache_err = esp_cache_msync(sun_pixels, sun_size,
                                          ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                                          ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    if (cache_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_cache_msync(sun) failed: %d", cache_err);
        return;
    }
    cache_err = esp_cache_msync(mountain_pixels, mountain_size,
                                ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                                ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    if (cache_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_cache_msync(mountain) failed: %d", cache_err);
        return;
    }

    // PPA clients -- one per op type. The same callback feeds the
    // shared counting semaphore for all three; the main loop submits
    // FILL+SRM+BLEND in order each frame and takes the semaphore
    // three times before the foreground render passes start.
    ppa_done_sem = xSemaphoreCreateCounting(8, 0);
    if (ppa_done_sem == NULL) {
        ESP_LOGE(TAG, "Failed to create PPA semaphore");
        return;
    }
    ppa_event_callbacks_t const ppa_cbs = {.on_trans_done = ppa_on_trans_done};
    ppa_client_config_t   const srm_cfg = {
          .oper_type             = PPA_OPERATION_SRM,
          .max_pending_trans_num = 1,
          .data_burst_length     = PPA_DATA_BURST_LENGTH_128,
    };
    ppa_client_config_t const blend_cfg = {
        .oper_type             = PPA_OPERATION_BLEND,
        .max_pending_trans_num = 1,
        .data_burst_length     = PPA_DATA_BURST_LENGTH_128,
    };
    ppa_client_config_t const fill_cfg = {
        .oper_type             = PPA_OPERATION_FILL,
        .max_pending_trans_num = 1,
        .data_burst_length     = PPA_DATA_BURST_LENGTH_128,
    };
    if (ppa_register_client(&srm_cfg,   &ppa_srm_client)   != ESP_OK ||
        ppa_register_client(&blend_cfg, &ppa_blend_client) != ESP_OK ||
        ppa_register_client(&fill_cfg,  &ppa_fill_client)  != ESP_OK) {
        ESP_LOGE(TAG, "ppa_register_client failed");
        return;
    }
    if (ppa_client_register_event_callbacks(ppa_srm_client,   &ppa_cbs) != ESP_OK ||
        ppa_client_register_event_callbacks(ppa_blend_client, &ppa_cbs) != ESP_OK ||
        ppa_client_register_event_callbacks(ppa_fill_client,  &ppa_cbs) != ESP_OK) {
        ESP_LOGE(TAG, "ppa_client_register_event_callbacks failed");
        return;
    }
}
