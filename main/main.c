// Race the Synth — Tanmatsu graceloader app.
//
// Phase 4: TITLE → PLAYING → GAME_OVER state machine with AABB
// collision against the world's obstacle pool. F1 exits to the
// launcher from any state. See DEVELOPMENT.md for the full plan.

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bsp/device.h"
#include "bsp/display.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "game.h"
#include "hal/lcd_types.h"
#include "icons.h"
#include "input.h"
#include "nvs_flash.h"
#include "pax_gfx.h"
#include "render.h"
#include "rendertext.h"
#include "synthwave.h"
#include "world.h"

// PPA clients — one per operation type because the driver ties a
// client handle to a single op. All three feed a shared counting
// semaphore via their `on_trans_done` callbacks: every frame we
// submit FILL+SRM+BLEND non-blocking, do CPU floor work, then take
// the semaphore three times before any foreground rendering touches
// the sky region. PPA is a single hardware engine, so the ops
// execute sequentially in submission order even though we kick
// them off in one burst.
static ppa_client_handle_t          ppa_srm_client   = NULL;
static ppa_client_handle_t          ppa_blend_client = NULL;
static ppa_client_handle_t          ppa_fill_client  = NULL;
static SemaphoreHandle_t            ppa_done_sem     = NULL;
static int                          ppa_pending_n    = 0;

// ESP32-P4 PSRAM L2 cache line size. Used for the aligned allocation
// of the layer caches and for `esp_cache_msync` operations.
#define PPA_PSRAM_CACHE_LINE 128

// Sky region in logical coordinates. `synthwave_draw_top_grid` paints
// the magenta line at y = HORIZON_LOGICAL_Y; `synthwave_step` paints
// the floor starting at y = HORIZON_LOGICAL_Y + 1. The PPA pipeline
// touches logical rows [0, HORIZON_LOGICAL_Y], i.e. SKY_ROWS rows.
#define HORIZON_LOGICAL_Y 256
#define SKY_ROWS          (HORIZON_LOGICAL_Y + 1)

// Sun cache: tight bounding box of the sun bands at their canonical
// baseline. Bands span fb logical y = -4 (off-screen above) to ~174;
// we render with y_bias = +4 so the topmost band lands at cache y=0
// and the cache is exactly tall enough to hold the whole sun.
#define SUN_CACHE_LOG_W   800
#define SUN_CACHE_LOG_H   180
#define SUN_RENDER_Y_BIAS 4.0f

// Mountain cache: tight bounding box of the visible mountain band.
// The band spans fb logical y = 94 (mountain peaks) down to 256
// (horizon). The cache is rendered with y_bias = -94 so the top of
// the visible mountain region lands at cache y=0, and the horizon
// line at fb y=256 lands at cache y=162.
#define MOUNTAIN_CACHE_LOG_W   800
#define MOUNTAIN_CACHE_LOG_H   163
#define MOUNTAIN_RENDER_Y_BIAS (-94.0f)
#define MOUNTAIN_DEST_LOG_Y    94

// Colour-key for the mountain cache background. Pure green never
// appears in the synthwave palette (purples / pinks / cyans /
// oranges / magentas), so a tight key around it can't false-match
// any artwork pixel. The cache stores RGB565 (5-6-5); PPA expands
// to RGB888 internally before comparing against the thresholds.
// Whether the expansion is "shift" (g=0x3F → 0xFC) or "replicate"
// (g=0x3F → 0xFF) varies by hardware revision, so the threshold
// range covers both: low (0,0xFC,0) — high (0,0xFF,0).
#define MOUNTAIN_KEY_PAX_COL 0xFF00FF00u

// Sky colour for PPA FILL. Same purple PAX paints with
// `pax_background(0xFF552075)`.
#define SKY_PAX_COL 0xFF552075u

static char const TAG[] = "racethesynth";

static size_t                       display_h_res        = 0;
static size_t                       display_v_res        = 0;
static lcd_color_rgb_pixel_format_t display_color_format = LCD_COLOR_PIXEL_FORMAT_RGB888;
static lcd_rgb_data_endian_t        display_data_endian  = LCD_RGB_DATA_ENDIAN_LITTLE;
// Double-buffered framebuffer. The LCD's DMA reads `fb_front`
// continuously while the CPU + PPA draw into `*fb` (the back buffer).
// `bsp_display_blit` is called with the current back buffer pointer;
// after the post-blit vsync wait, the two buffers swap so the buffer
// just sent becomes the front (being scanned) and the previous front
// becomes the back (next frame's draw target). This eliminates the
// CPU-modifies-while-LCD-reads tearing that single-buffering caused.
static pax_buf_t                    fb_a                 = {0};
static pax_buf_t                    fb_b                 = {0};
static pax_buf_t*                   fb                   = &fb_a;
static pax_buf_t*                   fb_front             = &fb_b;
static void*                        fb_a_pixels          = NULL;
static void*                        fb_b_pixels          = NULL;
static size_t                       fb_size              = 0;

// Pre-rendered backdrop layers, split into two PPA-driven caches so
// the sun can move independently of the mountains. The per-frame
// pipeline is:
//   1) PPA FILL — sky purple across the whole above-horizon region.
//      Wipes any previous-frame obstacle pixels that drifted up
//      into the sky area.
//   2) PPA SRM  — copy `sun_cache` into fb at the sun's current
//      vertical offset. Cache contents are sky-color in the gaps
//      around the bands, so the SRM is harmless outside the bands
//      (writes the same purple the FILL already wrote).
//   3) PPA BLEND — composite `mountain_cache` over fb with a green
//      colour-key so the sky/sun shows through outside the mountain
//      silhouette. Mountain wireframes and the horizon line live in
//      the same cache.
static pax_buf_t                    sun_cache            = {0};
static void*                        sun_pixels           = NULL;
static size_t                       sun_size             = 0;
static pax_buf_t                    mountain_cache       = {0};
static void*                        mountain_pixels      = NULL;
static size_t                       mountain_size        = 0;

typedef enum {
    APP_STATE_TITLE = 0,
    APP_STATE_PLAYING,
    APP_STATE_GAME_OVER,
} app_state_t;

static void blit(void) {
    bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(fb));
}

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
// raw memory; PAX rotates logical → raw by `rx = raw_w - 1 - ly`,
// `ry = lx`. So a logical rectangle of (lx0..lx1, ly0..ly1) becomes
// raw block (rx_start = raw_w - 1 - ly1, ry_start = lx0) with
// block_w = ly1 - ly0 + 1 (height in logical → width in raw) and
// block_h = lx1 - lx0 + 1 (width in logical → height in raw).
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
// into the corresponding raw block for a buffer of size raw_w × raw_h
// with PAX_O_ROT_CW. The logical x range is always [0, 800), full
// width.
static ppa_raw_blk_t ppa_band_to_raw(uint32_t raw_w, uint32_t raw_h, int log_y_top, int log_h) {
    ppa_raw_blk_t b;
    b.pic_w          = raw_w;
    b.pic_h          = raw_h;
    b.block_w        = (uint32_t)log_h;          // logical-h → raw-w
    b.block_h        = raw_h;                    // logical x range [0, 800) → full raw_h
    b.block_offset_x = (uint32_t)((int)raw_w - log_y_top - log_h);
    b.block_offset_y = 0;
    return b;
}

// Submit PPA FILL: paint the sky region of fb with the sky purple.
// Wipes any previous-frame obstacle pixels that drifted above the
// horizon, so the sun and mountain layers compose onto a clean band.
static bool ppa_submit_fill_sky(void) {
    ppa_raw_blk_t const dst = ppa_band_to_raw((uint32_t)display_h_res, (uint32_t)display_v_res, 0, SKY_ROWS);
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

// Submit PPA SRM: copy the sun cache onto fb starting at the sun's
// current top y in logical coordinates. For now sun_dy is always 0
// (the sunset mechanic lands in Phase 5); pass the desired top y
// directly to drive the SRM destination.
static bool ppa_submit_sun(int dest_top_log_y) {
    uint32_t const fb_raw_w   = (uint32_t)display_h_res;
    uint32_t const fb_raw_h   = (uint32_t)display_v_res;
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

// Submit PPA BLEND: composite the mountain cache over fb with a
// colour-key on pure green so the sky/sun shows through outside the
// mountain silhouette. Mountains and wireframes and the horizon line
// are all baked into the same cache.
static bool ppa_submit_mountains(void) {
    uint32_t const fb_raw_w   = (uint32_t)display_h_res;
    uint32_t const fb_raw_h   = (uint32_t)display_v_res;
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
        // transparent — the background passes through. The window
        // covers both possible 565→888 expansion modes (shift gives
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

// Wait for one pending PPA op to complete. Used to enforce strict
// submission order between ops on different clients (the driver does
// not guarantee FIFO across client boundaries — observed empirically
// as sun/mountain z-fighting when SRM and BLEND submissions race).
static void ppa_wait_one(void) {
    if (ppa_pending_n <= 0) return;
    if (xSemaphoreTake(ppa_done_sem, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "PPA wait_one timed out (%d pending)", ppa_pending_n);
        return;
    }
    ppa_pending_n--;
}


static void draw_centered(float cy, float h, pax_col_t color, char const* text) {
    pax_vec2f sz = rendertext_size(NULL, h, text);
    float const x = (pax_buf_get_widthf(fb) - sz.x) * 0.5f;
    rendertext_draw(fb, color, NULL, h, x, cy, text);
}

static void draw_speed_readout(float speed_z) {
    char        buf[32];
    snprintf(buf, sizeof(buf), "v=%.1f", speed_z);
    float const text_h = 18.0f;
    pax_vec2f   sz     = rendertext_size(NULL, text_h, buf);
    float const x      = pax_buf_get_widthf(fb) - sz.x - 12.0f;
    rendertext_draw(fb, 0xFFFFFFFF, NULL, text_h, x, 12.0f, buf);
}

static void draw_exit_hint(void) {
    char const* prompt   = "to exit";
    float const prompt_h = 18.0f;
    int         icon_w   = icons_width(ICON_F1);
    float const x_margin = 12.0f;
    float const y        = 12.0f;
    if (icon_w > 0) {
        float const gap    = 8.0f;
        int         icon_h = icons_height(ICON_F1);
        float       icon_y = y + prompt_h / 2.0f - (float)icon_h / 2.0f;
        float       text_x = x_margin + (float)icon_w + gap;
        icons_blit(fb, ICON_F1, x_margin, icon_y);
        rendertext_draw(fb, 0xFFFFFFFF, NULL, prompt_h, text_x, y, prompt);
    } else {
        char const* fallback = "F1 to exit";
        rendertext_draw(fb, 0xFFFFFFFF, NULL, prompt_h, x_margin, y, fallback);
    }
}

static void draw_title_overlay(void) {
    float const fbh = pax_buf_get_heightf(fb);
    draw_centered(fbh * 0.30f, 64.0f, 0xFFFFFF6Bu, "RACE THE SYNTH");
    draw_centered(fbh * 0.52f, 22.0f, 0xFFFFFFFFu, "press space to start");
}

static void draw_game_over_overlay(void) {
    float const fbh = pax_buf_get_heightf(fb);
    draw_centered(fbh * 0.30f, 64.0f, 0xFFF71FF1u, "GAME OVER");
    draw_centered(fbh * 0.52f, 22.0f, 0xFFFFFFFFu, "press space to retry");
}

// Build the daily seed from the RTC date — `year*10000 + month*100
// + day`. Same date → same seed → identical world, regardless of
// how many times the player restarts the run or the app. The seed
// only ticks over when the calendar day rolls over. If the RTC is
// unset (year < 2024) we fall back to a fixed constant so the run
// is still reproducible within a boot; Phase 8's NVS anti-cheat
// will replace that fallback with a stored last-known-good date.
//
// Phase 8 will also add an opt-in custom-seed menu on the title
// screen; that path bypasses this function and passes its own seed
// into start_run().
static uint32_t derive_daily_seed(void) {
    time_t    now = time(NULL);
    struct tm lt  = {0};
    localtime_r(&now, &lt);
    int const year = lt.tm_year + 1900;
    if (year < 2024) {
        return 1u;
    }
    int const month = lt.tm_mon + 1;
    int const day   = lt.tm_mday;
    uint32_t  seed  = (uint32_t)year * 10000u + (uint32_t)month * 100u + (uint32_t)day;
    return seed ? seed : 1u;
}

// Reset run state and (re-)seed the world. The seed is supplied by
// the caller so the same world replays exactly on retry. The caller
// owns the seed source — daily seed today, daily + custom seed once
// Phase 8 lands.
static void start_run(game_state_t* game, world_state_t* world, uint32_t seed) {
    game_init(game);
    world_init(world, seed);
    input_set_mode(INPUT_MODE_PLAYING);
}

void app_main(void) {
    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        res = nvs_flash_init();
    }
    ESP_ERROR_CHECK(res);

    bsp_configuration_t const bsp_configuration = {
        .display =
            {
                .requested_color_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
                .num_fbs                = 1,
            },
    };
    res = bsp_device_initialize(&bsp_configuration);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize BSP: %d", res);
        return;
    }

    res = bsp_display_get_parameters(&display_h_res, &display_v_res, &display_color_format, &display_data_endian);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get display parameters: %d", res);
        return;
    }

    pax_buf_type_t format = PAX_BUF_24_888RGB;
    switch (display_color_format) {
        case LCD_COLOR_PIXEL_FORMAT_RGB565:
            format = PAX_BUF_16_565RGB;
            break;
        case LCD_COLOR_PIXEL_FORMAT_RGB888:
            format = PAX_BUF_24_888RGB;
            break;
        default:
            break;
    }

    bsp_display_rotation_t display_rotation = bsp_display_get_default_rotation();
    pax_orientation_t      orientation      = PAX_O_UPRIGHT;
    switch (display_rotation) {
        case BSP_DISPLAY_ROTATION_90:
            orientation = PAX_O_ROT_CCW;
            break;
        case BSP_DISPLAY_ROTATION_180:
            orientation = PAX_O_ROT_HALF;
            break;
        case BSP_DISPLAY_ROTATION_270:
            orientation = PAX_O_ROT_CW;
            break;
        case BSP_DISPLAY_ROTATION_0:
        default:
            orientation = PAX_O_UPRIGHT;
            break;
    }

    // Allocate both framebuffers in PSRAM with PPA-cache-line
    // alignment. Each is wrapped in its own pax_buf_t so PAX rasterises
    // straight into the raw layout the LCD (and PPA) will read. The
    // `fb` pointer tracks the current back buffer; `fb_front` tracks
    // the buffer last handed to bsp_display_blit (= currently being
    // scanned out by the LCD). The two swap each frame after blit +
    // vsync — at any moment the CPU and PPA only touch `*fb`, never
    // the one the LCD is reading.
    fb_size = (size_t)display_h_res * display_v_res * 2u;
    size_t const aligned_fb_size = (fb_size + PPA_PSRAM_CACHE_LINE - 1) & ~(size_t)(PPA_PSRAM_CACHE_LINE - 1);
    fb_a_pixels = heap_caps_aligned_alloc(PPA_PSRAM_CACHE_LINE, aligned_fb_size, MALLOC_CAP_SPIRAM);
    fb_b_pixels = heap_caps_aligned_alloc(PPA_PSRAM_CACHE_LINE, aligned_fb_size, MALLOC_CAP_SPIRAM);
    if (fb_a_pixels == NULL || fb_b_pixels == NULL) {
        ESP_LOGE(TAG, "Failed to allocate framebuffers (a=%p b=%p)", fb_a_pixels, fb_b_pixels);
        return;
    }

    pax_buf_init(&fb_a, fb_a_pixels, display_h_res, display_v_res, format);
    pax_buf_reversed(&fb_a, display_data_endian == LCD_RGB_DATA_ENDIAN_BIG);
    pax_buf_set_orientation(&fb_a, orientation);

    pax_buf_init(&fb_b, fb_b_pixels, display_h_res, display_v_res, format);
    pax_buf_reversed(&fb_b, display_data_endian == LCD_RGB_DATA_ENDIAN_BIG);
    pax_buf_set_orientation(&fb_b, orientation);

    synthwave_init();
    icons_load();
    input_init();
    input_set_mode(INPUT_MODE_TITLE);

    // Layer caches: tight bounding boxes for the sun bands and the
    // visible mountain band. Both live in PSRAM, both are aligned to
    // the PPA cache-line requirement (128 B on ESP32-P4 external mem).
    // Allocation size is rounded *up* to the cache line so the
    // tail of the buffer doesn't share a line with neighbouring
    // allocations — picture dimensions stay tight regardless.
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
    // 800-wide × 180-tall cache needs a 180-wide × 800-tall raw
    // buffer. Passing the logical dims here would give PAX an
    // 800-wide × 180-tall raw buffer, then ROT_CW would swap them
    // back to 180 logical wide × 800 logical tall — every sun-band
    // x coordinate (294..506) would then clip out of bounds.
    pax_buf_init(&sun_cache, sun_pixels, SUN_CACHE_LOG_H, SUN_CACHE_LOG_W, format);
    pax_buf_reversed(&sun_cache, display_data_endian == LCD_RGB_DATA_ENDIAN_BIG);
    pax_buf_set_orientation(&sun_cache, orientation);

    pax_buf_init(&mountain_cache, mountain_pixels, MOUNTAIN_CACHE_LOG_H, MOUNTAIN_CACHE_LOG_W, format);
    pax_buf_reversed(&mountain_cache, display_data_endian == LCD_RGB_DATA_ENDIAN_BIG);
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
    // pixels. One-shot at boot — neither cache changes after this.
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

    // PPA clients — one per op type. The same callback feeds the
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

    SemaphoreHandle_t vsync_sem = NULL;
    esp_err_t         te_err    = bsp_display_set_tearing_effect_mode(BSP_DISPLAY_TE_V_BLANKING);
    if (te_err == ESP_OK) {
        te_err = bsp_display_get_tearing_effect_semaphore(&vsync_sem);
    }
    if (te_err != ESP_OK || vsync_sem == NULL) {
        ESP_LOGW(TAG, "Vsync not available — animation may stutter");
        vsync_sem = NULL;
    }

    // World state is ~5 KB at the current pool size — keep it off
    // the app_main stack so we don't have to worry about IDF's
    // default stack budget.
    static game_state_t  game;
    static world_state_t world;
    game_init(&game);

    // Daily seed. Derived from today's calendar date so every run
    // — across restarts, across app reboots — uses the same world
    // layout until the next midnight rollover. Phase 8 will add an
    // opt-in custom-seed menu and the anti-cheat fallback for an
    // unset RTC.
    uint32_t const run_seed = derive_daily_seed();
    // The title screen scrolls the floor with a fake speed so the
    // scene reads as "live" instead of static. The world isn't
    // advanced (no obstacles spawn yet) — start_run() initializes
    // the world fresh each time PLAYING begins.
    float const title_scroll_speed = 6.0f;

    app_state_t app_state = APP_STATE_TITLE;
    int64_t     prev_us   = esp_timer_get_time();

    // Per-frame timing accumulators (microseconds), summed over a
    // ~1 s window then logged + reset. Phases are mutually exclusive
    // and together cover the whole loop iteration, so the sum should
    // approximately equal `window_us`.
    int64_t prof_input_us  = 0;
    int64_t prof_phys_us   = 0;
    int64_t prof_bgkick_us = 0;
    int64_t prof_bgflr_us  = 0;
    int64_t prof_bgwait_us = 0;
    int64_t prof_obs_us    = 0;
    int64_t prof_fgrest_us = 0;
    int64_t prof_blit_us   = 0;
    int64_t prof_vsync_us  = 0;
    int     prof_frames    = 0;
    int64_t prof_window_us = 0;
    int64_t prof_prev_us   = prev_us;

    ESP_LOGI(TAG, "Race the Synth: title screen up");

    while (1) {
        int64_t const t_loop_start = esp_timer_get_time();

        if (input_drain_events()) {
            bsp_device_restart_to_launcher();
        }

        int64_t now_us = t_loop_start;
        float   dt     = (float)(now_us - prev_us) / 1e6f;
        prev_us        = now_us;
        if (dt > 0.1f) dt = 0.1f;

        // Debug speed knob acts on the *base* speed so the scrape
        // ramps don't fight the player's tuning.
        int sd = input_consume_speed_delta();
        if (sd != 0) {
            game.ship_base_speed_z += (float)sd * 1.0f;
            if (game.ship_base_speed_z < 0.5f) game.ship_base_speed_z = 0.5f;
            if (game.ship_base_speed_z > 60.0f) game.ship_base_speed_z = 60.0f;
        }

        bool const pickup_pressed = input_consume_pickup();
        int  const steer          = input_steering();

        int64_t const t_after_input = esp_timer_get_time();
        bool          head_on       = false;

        // Physics pass — only meaningful in PLAYING; the other states
        // record zero physics time so the breakdown stays honest.
        if (app_state == APP_STATE_PLAYING) {
            // 1. Apply bank + lateral motion using this frame's steer.
            // 2. Collide: push the ship out of side-contact obstacles
            //    and set scrape flags (or return head-on).
            // 3. After-collide work that reads the flags: ramp speed,
            //    emit + advance sparks.
            game_step(&game, dt, steer);
            head_on = game_collide(&game, &world, dt);
            game_after_collide(&game, dt);
            world_advance(&world, dt, game.ship_speed_z);
        }
        int64_t const t_after_phys = esp_timer_get_time();

        // Background pass — FILL → SRM → BLEND, explicitly serialised.
        // PPA's dispatch order across different clients is *not*
        // guaranteed to match submission order: empirically the SRM
        // (sun) and BLEND (mountain) ops raced and the sun
        // overwrote the mountains in some frames. Adding a wait
        // between each submit pins the order at the cost of losing
        // CPU/PPA parallelism for the first two ops. The CPU floor
        // work still runs in parallel with the BLEND (the longest
        // single op besides the CPU work itself), so the bg-phase
        // wallclock is FILL + SRM + max(BLEND, floor).
        //
        // FILL is the per-frame guarantee that no stale obstacle
        // pixel from the previous frame remains in the sky band.
        ppa_submit_fill_sky();
        ppa_wait_one();
        ppa_submit_sun(0);                  // sun_dy = 0 until Phase 5
        ppa_wait_one();
        ppa_submit_mountains();
        int64_t const t_after_bgkick = esp_timer_get_time();
        float const floor_scroll = (app_state == APP_STATE_TITLE)      ? title_scroll_speed * dt
                                   : (app_state == APP_STATE_PLAYING)  ? game.ship_speed_z * dt
                                                                       : 0.0f;
        float const floor_cam_x  = (app_state == APP_STATE_TITLE) ? 0.0f : game.cam_x;
        synthwave_step(fb, floor_scroll, floor_cam_x);
        int64_t const t_after_bgflr = esp_timer_get_time();
        // Wait for the BLEND op to finish — obstacles and HUD text
        // can both write into the sky region, so the backdrop must
        // be in place before any foreground render touches it.
        ppa_wait_one();
        int64_t const t_after_bg = esp_timer_get_time();

        // Foreground pass — state-dependent dynamic content.
        // `obs` measures render_obstacles in isolation since it
        // dominates the gameplay frame; everything else (ship,
        // sparks, HUD, overlays) rolls up under `fgrest`.
        int64_t t_after_obs = 0;
        switch (app_state) {
            case APP_STATE_TITLE: {
                t_after_obs = esp_timer_get_time();
                draw_title_overlay();
                draw_exit_hint();
                if (pickup_pressed) {
                    start_run(&game, &world, run_seed);
                    app_state = APP_STATE_PLAYING;
                }
                break;
            }

            case APP_STATE_PLAYING: {
                render_obstacles(fb, &world, game.cam_x);
                t_after_obs = esp_timer_get_time();
                game_draw_ship(fb, &game);
                game_draw_sparks(fb, &game);
                draw_exit_hint();
                draw_speed_readout(game.ship_speed_z);

                if (head_on) {
                    app_state = APP_STATE_GAME_OVER;
                    input_set_mode(INPUT_MODE_GAME_OVER);
                }
                break;
            }

            case APP_STATE_GAME_OVER: {
                // World frozen at the crash. No sparks here — they
                // are a per-frame radial flash that only reads as
                // a scrape indication during PLAYING.
                render_obstacles(fb, &world, game.cam_x);
                t_after_obs = esp_timer_get_time();
                game_draw_ship(fb, &game);
                draw_game_over_overlay();
                draw_exit_hint();

                if (pickup_pressed) {
                    start_run(&game, &world, run_seed);
                    app_state = APP_STATE_PLAYING;
                }
                break;
            }
        }
        int64_t const t_after_fg = esp_timer_get_time();

        blit();
        int64_t const t_after_blit = esp_timer_get_time();

        if (vsync_sem != NULL) {
            xSemaphoreTake(vsync_sem, pdMS_TO_TICKS(50));
        } else {
            vTaskDelay(pdMS_TO_TICKS(16));
        }
        int64_t const t_after_vsync = esp_timer_get_time();

        // Swap back/front: the buffer we just blitted is now the
        // front (the LCD scans it out); the previous front becomes
        // the new back (next frame's draw target). Both PPA and CPU
        // only ever touch `*fb`, so the LCD's scan-out buffer is
        // never modified mid-flight — no more tearing.
        pax_buf_t* tmp = fb;
        fb             = fb_front;
        fb_front       = tmp;

        prof_input_us  += t_after_input  - t_loop_start;
        prof_phys_us   += t_after_phys   - t_after_input;
        prof_bgkick_us += t_after_bgkick - t_after_phys;
        prof_bgflr_us  += t_after_bgflr  - t_after_bgkick;
        prof_bgwait_us += t_after_bg     - t_after_bgflr;
        prof_obs_us    += t_after_obs    - t_after_bg;
        prof_fgrest_us += t_after_fg     - t_after_obs;
        prof_blit_us   += t_after_blit   - t_after_fg;
        prof_vsync_us  += t_after_vsync  - t_after_blit;
        prof_frames    += 1;
        prof_window_us  = t_after_vsync - prof_prev_us;

        if (prof_window_us >= 1000000) {
            float const fps    = prof_frames * 1e6f / (float)prof_window_us;
            float const inv_fr = 1.0f / (float)prof_frames;
            ESP_LOGI(TAG,
                     "FPS=%.1f  in=%.2f phys=%.2f bgkick=%.2f bgflr=%.2f bgwait=%.2f obs=%.2f fgrest=%.2f blit=%.2f vsync=%.2f ms",
                     fps,
                     (float)prof_input_us  * inv_fr / 1000.0f,
                     (float)prof_phys_us   * inv_fr / 1000.0f,
                     (float)prof_bgkick_us * inv_fr / 1000.0f,
                     (float)prof_bgflr_us  * inv_fr / 1000.0f,
                     (float)prof_bgwait_us * inv_fr / 1000.0f,
                     (float)prof_obs_us    * inv_fr / 1000.0f,
                     (float)prof_fgrest_us * inv_fr / 1000.0f,
                     (float)prof_blit_us   * inv_fr / 1000.0f,
                     (float)prof_vsync_us  * inv_fr / 1000.0f);
            prof_input_us  = prof_phys_us = prof_bgkick_us = prof_bgflr_us = 0;
            prof_bgwait_us = prof_obs_us = prof_fgrest_us = prof_blit_us = prof_vsync_us = 0;
            prof_frames    = 0;
            prof_prev_us   = t_after_vsync;
        }
    }
}
