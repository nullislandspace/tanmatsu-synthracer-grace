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
#include "direct_565.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "audio_mixer.h"
#include "audio_settings.h"
#include "game.h"
#include "hal/lcd_types.h"
#include "icons.h"
#include "input.h"
#include "magicnumbers.h"
#include "music/music_procedural.h"
#include "nvs_flash.h"
#include "pax_gfx.h"
#include "render.h"
#include "rendertext.h"
#include "save.h"
#include "sfx/sfx_crash.h"
#include "sfx/sfx_engine_hum.h"
#include "sfx/sfx_pickup_ding.h"
#include "sfx/sfx_scrape.h"
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
#define SUN_CACHE_LOG_W   DISPLAY_LOG_W
#define SUN_CACHE_LOG_H   180
#define SUN_RENDER_Y_BIAS 4.0f

// Mountain cache: tight bounding box of the visible mountain band.
// The band spans fb logical y = 94 (mountain peaks) down to 256
// (horizon). The cache is rendered with y_bias = -94 so the top of
// the visible mountain region lands at cache y=0, and the horizon
// line at fb y=256 lands at cache y=162.
#define MOUNTAIN_CACHE_LOG_W   DISPLAY_LOG_W
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
    APP_STATE_SLOT_SELECT = 0,  // first state on boot: pick save slot 0..2
    APP_STATE_MENU,             // main menu: Daily/Seeded/Upgrade/Stats/Audio/Exit
    APP_STATE_SEED_INPUT,       // numeric entry for the custom seed
    APP_STATE_STATS_VIEW,       // text dump of the active slot's stats
    APP_STATE_UPGRADE_STUB,     // placeholder "coming soon" screen
    APP_STATE_AUDIO_SETTINGS,   // two-checkbox panel: music / SFX on-off
    APP_STATE_PLAYING,
    APP_STATE_PAUSED,           // F4 pause overlay: Resume / Abort run
    APP_STATE_GAME_OVER,
} app_state_t;

// Pause-menu entries (STATE_PAUSED).
enum {
    PAUSE_ENTRY_RESUME = 0,
    PAUSE_ENTRY_ABORT,
    PAUSE_ENTRY_COUNT,
};

// Menu entry indices for STATE_MENU. Order is the visible order.
enum {
    MENU_ENTRY_DAILY = 0,
    MENU_ENTRY_SEEDED,
    MENU_ENTRY_UPGRADE,
    MENU_ENTRY_STATS,
    MENU_ENTRY_AUDIO,
    MENU_ENTRY_EXIT,
    MENU_ENTRY_COUNT,
};

// Audio-settings cursor entries (STATE_AUDIO_SETTINGS).
enum {
    AUDIO_ENTRY_MUSIC = 0,
    AUDIO_ENTRY_SFX,
    AUDIO_ENTRY_COUNT,
};
static int s_audio_cursor = AUDIO_ENTRY_MUSIC;

// Persistence state owned by main. The active slot is sticky for the
// app lifetime; we load on slot select and write after every run.
static save_data_t s_save        = {0};
static int         s_active_slot = -1;

// Menu / seed-input cursor state. Each is the per-screen cursor for
// the corresponding STATE_* entry — main loop resets the cursor on
// entry to each screen.
static int  s_slot_cursor  = 0;            // STATE_SLOT_SELECT cursor (0..2)
static int  s_menu_cursor  = MENU_ENTRY_DAILY;
static int  s_pause_cursor = PAUSE_ENTRY_RESUME;
static char s_seed_buf[11] = {0};          // STATE_SEED_INPUT decimal seed (max 10 digits)
static int  s_seed_len     = 0;

// Game-over flavour text pool. The displayed line is chosen on the
// PLAYING → GAME_OVER transition and stays put until the next run
// ends; same flavour is rendered on every frame while GAME_OVER is
// active. Add more lines by appending to the array — the count is
// derived from `sizeof`, no manual update needed.
static char const* const gameover_flavours[] = {
    "Failure. Expected and inevitable.",
    "Death. The Great Divide.",
    "Infinity failed. Things end.",
};
#define GAMEOVER_FLAVOUR_COUNT  ((int)(sizeof(gameover_flavours) / sizeof(gameover_flavours[0])))

static int s_gameover_flavour_idx = 0;

// Run-instrumentation captured at run start, used by
// save_commit_run_end on transition into GAME_OVER. `s_run_play_seconds`
// accumulates per-PLAYING-frame `dt` so paused time is excluded from
// the saved duration stat (wallclock since start would count F4
// pauses in the play time, which we don't want).
static int64_t s_run_started_us   = 0;
static double  s_run_play_seconds = 0.0;
static int     s_peak_stage       = 1;
static int     s_run_was_custom   = 0;
static int64_t s_run_seed_used    = 0;

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

// Top-right readout stack. Slot 0 = score, slot 1 = stage, slot 2 = v,
// slot 3 = sun. Each line is `text_h + 4` px below the previous.
static void draw_speed_readout(float speed_z) {
    char        buf[32];
    snprintf(buf, sizeof(buf), "v=%.1f", speed_z);
    float const text_h = 18.0f;
    pax_vec2f   sz     = rendertext_size(NULL, text_h, buf);
    float const x      = pax_buf_get_widthf(fb) - sz.x - 12.0f;
    rendertext_draw(fb, 0xFFFFFFFF, NULL, text_h, x, 12.0f + 2.0f * (text_h + 4.0f), buf);
}

static void draw_sun_readout(float sun_y) {
    char        buf[32];
    snprintf(buf, sizeof(buf), "sun=%.1f", sun_y);
    float const text_h = 18.0f;
    pax_vec2f   sz     = rendertext_size(NULL, text_h, buf);
    float const x      = pax_buf_get_widthf(fb) - sz.x - 12.0f;
    rendertext_draw(fb, 0xFFFFFFFF, NULL, text_h, x, 12.0f + 3.0f * (text_h + 4.0f), buf);
}

// Bottom-left HUD: a solid green upward-pointing triangle that's
// visible whenever a boost is active (any phase that isn't IDLE).
// Sized at 3× the debug-readout text height so it's easy to read
// out of the corner of the eye while flying. Drawn via the direct
// 565 rasterizer — single triangle, ~zero cost.
static void draw_boost_indicator(game_state_t const* g) {
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

// F4-to-pause hint, drawn below the F1 exit hint during PLAYING.
// Same icon-then-text layout as draw_exit_hint, slotted at y = 34
// (= 12 + 18 + 4) so it sits one line below F1.
static void draw_pause_hint(void) {
    char const* prompt   = "to pause";
    float const prompt_h = 18.0f;
    int         icon_w   = icons_width(ICON_F4);
    float const x_margin = 12.0f;
    float const y        = 12.0f + (prompt_h + 4.0f);
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

// Translucent dim panel behind menu text. Centred rectangle sized by
// the caller — leaves a synthwave border at the edges so the menu
// still reads as overlaid on the live scene rather than a context
// switch. Each menu's draw_*() picks dimensions sized to its text
// extents (footer hint baseline + title baseline + line spacing).
static void draw_menu_panel_size(float w_frac, float h_frac) {
    float const fbw = pax_buf_get_widthf(fb);
    float const fbh = pax_buf_get_heightf(fb);
    int   const pw  = (int)(fbw * w_frac);
    int   const ph  = (int)(fbh * h_frac);
    int   const px  = (int)((fbw - (float)pw) * 0.5f);
    int   const py  = (int)((fbh - (float)ph) * 0.5f);
    uint16_t* const pixels = (uint16_t*)pax_buf_get_pixels(fb);
    direct_565_dim_rect(pixels, fb->reverse_endianness, px, py, pw, ph);
}

// Smaller dim panel for the pause overlay — sized to fit "PAUSED" +
// two entries + footer hint. Keeps the gameplay scene mostly visible
// so the player can see roughly where they were when they paused.
// Custom centring (sits a little higher than dead-centre).
static void draw_pause_panel(void) {
    float const fbw = pax_buf_get_widthf(fb);
    float const fbh = pax_buf_get_heightf(fb);
    int   const pw  = (int)(fbw * 0.55f);
    int   const ph  = (int)(fbh * 0.58f);
    int   const px  = (int)((fbw - (float)pw) * 0.5f);
    int   const py  = (int)(fbh * 0.22f);
    uint16_t* const pixels = (uint16_t*)pax_buf_get_pixels(fb);
    direct_565_dim_rect(pixels, fb->reverse_endianness, px, py, pw, ph);
}

static void draw_game_over_overlay(void) {
    // Panel sized to bound "GAME OVER" (64 px, top 30 %) + flavour
    // text (22 px, top 46 %) + "press space to retry" (22 px, top
    // 58 %). Width widened to fit the longest flavour line at the
    // current text size.
    float const fbw = pax_buf_get_widthf(fb);
    float const fbh = pax_buf_get_heightf(fb);
    int   const pw  = (int)(fbw * 0.66f);
    int   const ph  = (int)(fbh * 0.46f);
    int   const px  = (int)((fbw - (float)pw) * 0.5f);
    int   const py  = (int)(fbh * 0.22f);
    uint16_t* const pixels = (uint16_t*)pax_buf_get_pixels(fb);
    direct_565_dim_rect(pixels, fb->reverse_endianness, px, py, pw, ph);

    draw_centered(fbh * 0.30f, 64.0f, 0xFFF71FF1u, "GAME OVER");
    draw_centered(fbh * 0.46f, 22.0f, 0xFF31FBFBu, gameover_flavours[s_gameover_flavour_idx]);
    draw_centered(fbh * 0.58f, 22.0f, 0xFFFFFFFFu, "press space to retry");
}

// Format an int64 Unix time as "YYYY-MM-DD HH:MM" into out. Empty
// string if t == 0.
static void format_unix(int64_t t, char* out, size_t out_size) {
    if (t <= 0) {
        snprintf(out, out_size, "—");
        return;
    }
    time_t    tt = (time_t)t;
    struct tm lt = {0};
    localtime_r(&tt, &lt);
    snprintf(out, out_size, "%04d-%02d-%02d %02d:%02d",
             lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
             lt.tm_hour, lt.tm_min);
}

// Slot-select screen — three rows with summary stats (or [new] when
// the slot has no save file).
static void draw_slot_select(void) {
    // Title at 12%, rows from 34%, footer hint at 92% + 14 px → ~96%.
    draw_menu_panel_size(0.80f, 0.94f);
    float const fbh = pax_buf_get_heightf(fb);
    float const fbw = pax_buf_get_widthf(fb);
    draw_centered(fbh * 0.12f, 48.0f, 0xFFFFFF6Bu, "RACE THE SYNTH");
    draw_centered(fbh * 0.22f, 22.0f, 0xFFFFFFFFu, "select save slot");

    float const row_h = 56.0f;
    float const top   = fbh * 0.34f;
    for (int i = 0; i < SAVE_SLOT_COUNT; i++) {
        save_peek_info_t info  = {0};
        int              exist = save_slot_peek(i, &info) == 0;
        bool const       sel   = (i == s_slot_cursor);
        pax_col_t        title_col = sel ? 0xFFFFFF6Bu : 0xFFFFFFFFu;
        pax_col_t        sub_col   = sel ? 0xFFFFFFFFu : 0xFF808088u;

        char title[64];
        snprintf(title, sizeof(title), "%s slot %d %s",
                 sel ? ">" : " ", i + 1, sel ? "<" : " ");

        char sub[128];
        if (exist) {
            char when[64];
            format_unix(info.last_played_unix, when, sizeof(when));
            snprintf(sub, sizeof(sub),
                     "best %lld  stage %d  runs %d  %s",
                     (long long)info.score_best, (int)info.stage_best,
                     (int)info.runs_total, when);
        } else {
            snprintf(sub, sizeof(sub), "[new]");
        }

        float const y = top + (float)i * row_h;
        {
            pax_vec2f sz = rendertext_size(NULL, 24.0f, title);
            rendertext_draw(fb, title_col, NULL, 24.0f,
                            (fbw - sz.x) * 0.5f, y, title);
        }
        {
            pax_vec2f sz = rendertext_size(NULL, 16.0f, sub);
            rendertext_draw(fb, sub_col, NULL, 16.0f,
                            (fbw - sz.x) * 0.5f, y + 28.0f, sub);
        }
    }

    draw_centered(fbh * 0.92f, 14.0f, 0xFFA0A0A8u,
                  "up / down to choose, enter to confirm, F1 to exit");
}

// Main menu — five entries.
static void draw_main_menu(void) {
    draw_menu_panel_size(0.80f, 0.94f);
    char        title[64];
    snprintf(title, sizeof(title), "slot %d", s_active_slot + 1);
    float const fbh = pax_buf_get_heightf(fb);
    float const fbw = pax_buf_get_widthf(fb);
    draw_centered(fbh * 0.12f, 48.0f, 0xFFFFFF6Bu, "RACE THE SYNTH");
    draw_centered(fbh * 0.21f, 18.0f, 0xFFFFFFFFu, title);

    char const* labels[MENU_ENTRY_COUNT] = {
        [MENU_ENTRY_DAILY]   = "Daily Run",
        [MENU_ENTRY_SEEDED]  = "Seeded Run",
        [MENU_ENTRY_UPGRADE] = "Upgrade Ship",
        [MENU_ENTRY_STATS]   = "Stats",
        [MENU_ENTRY_AUDIO]   = "Audio",
        [MENU_ENTRY_EXIT]    = "Exit",
    };

    float const row_h = 44.0f;
    float const top   = fbh * 0.34f;
    for (int i = 0; i < MENU_ENTRY_COUNT; i++) {
        bool const sel = (i == s_menu_cursor);
        char line[64];
        snprintf(line, sizeof(line), "%s %s %s",
                 sel ? ">" : " ", labels[i], sel ? "<" : " ");
        pax_col_t col = sel ? 0xFFFFFF6Bu : 0xFFFFFFFFu;
        pax_vec2f sz  = rendertext_size(NULL, 28.0f, line);
        rendertext_draw(fb, col, NULL, 28.0f,
                        (fbw - sz.x) * 0.5f, top + (float)i * row_h, line);
    }

    draw_centered(fbh * 0.92f, 14.0f, 0xFFA0A0A8u,
                  "up / down to choose, enter to confirm");
}

// Audio-settings screen — two checkboxes, toggled with enter / space.
static void draw_audio_settings(void) {
    draw_menu_panel_size(0.60f, 0.70f);
    float const fbh = pax_buf_get_heightf(fb);
    float const fbw = pax_buf_get_widthf(fb);
    draw_centered(fbh * 0.20f, 36.0f, 0xFFFFFF6Bu, "Audio");

    char const* const labels[AUDIO_ENTRY_COUNT] = {
        [AUDIO_ENTRY_MUSIC] = "Music",
        [AUDIO_ENTRY_SFX]   = "Sound effects",
    };
    bool const states[AUDIO_ENTRY_COUNT] = {
        [AUDIO_ENTRY_MUSIC] = audio_settings_music_on(),
        [AUDIO_ENTRY_SFX]   = audio_settings_sfx_on(),
    };

    float const row_h = 44.0f;
    float const top   = fbh * 0.40f;
    for (int i = 0; i < AUDIO_ENTRY_COUNT; i++) {
        bool const sel = (i == s_audio_cursor);
        char line[64];
        snprintf(line, sizeof(line), "%s %s [%s]",
                 sel ? ">" : " ",
                 labels[i],
                 states[i] ? "X" : " ");
        pax_col_t col = sel ? 0xFFFFFF6Bu : 0xFFFFFFFFu;
        pax_vec2f sz  = rendertext_size(NULL, 28.0f, line);
        rendertext_draw(fb, col, NULL, 28.0f,
                        (fbw - sz.x) * 0.5f, top + (float)i * row_h, line);
    }

    draw_centered(fbh * 0.85f, 14.0f, 0xFFA0A0A8u,
                  "up / down to choose, enter to toggle, esc to leave");
}

// Seed-input screen — numeric entry, prefilled from last_custom_seed.
static void draw_seed_input(void) {
    draw_menu_panel_size(0.70f, 0.76f);
    float const fbh = pax_buf_get_heightf(fb);
    draw_centered(fbh * 0.20f, 36.0f, 0xFFFFFF6Bu, "Seeded Run");
    draw_centered(fbh * 0.34f, 18.0f, 0xFFFFFFFFu, "enter seed (digits 0-9)");

    char display[16];
    if (s_seed_len == 0) {
        snprintf(display, sizeof(display), "_");
    } else {
        snprintf(display, sizeof(display), "%s_", s_seed_buf);
    }
    draw_centered(fbh * 0.50f, 48.0f, 0xFFFFFF6Bu, display);

    draw_centered(fbh * 0.78f, 16.0f, 0xFFA0A0A8u,
                  "backspace edits, enter starts, esc cancels");
}

// Single-block stats renderer, run twice with different pointers
// (last_run, all_time) and labels. Returns the y of the next free
// line so the caller can stack blocks.
static float draw_stats_block(float tx, float y, char const* heading, run_stats_t const* rs) {
    float const text_h  = 18.0f;
    pax_col_t   hdr_col = 0xFFFFFF6Bu;
    pax_col_t   txt_col = 0xFFFFFFFFu;
    char        buf[128];

    rendertext_draw(fb, hdr_col, NULL, text_h, tx, y, heading); y += text_h + 4.0f;

    snprintf(buf, sizeof(buf), "  score        %lld",   (long long)rs->score);
    rendertext_draw(fb, txt_col, NULL, text_h, tx, y, buf); y += text_h + 2.0f;
    snprintf(buf, sizeof(buf), "  distance     %.1f u", rs->distance);
    rendertext_draw(fb, txt_col, NULL, text_h, tx, y, buf); y += text_h + 2.0f;
    snprintf(buf, sizeof(buf), "  stage        %d",     (int)rs->stage_reached);
    rendertext_draw(fb, txt_col, NULL, text_h, tx, y, buf); y += text_h + 2.0f;
    snprintf(buf, sizeof(buf), "  mult max     %dx",    (int)rs->multiplier_max);
    rendertext_draw(fb, txt_col, NULL, text_h, tx, y, buf); y += text_h + 2.0f;
    snprintf(buf, sizeof(buf), "  duration     %.1f s", rs->duration_s);
    rendertext_draw(fb, txt_col, NULL, text_h, tx, y, buf); y += text_h + 2.0f;
    snprintf(buf, sizeof(buf), "  pickups      boost %d  tri %d  jump %d  shield %d",
             (int)rs->pickups_speed_boost, (int)rs->pickups_tri,
             (int)rs->pickups_jump, (int)rs->pickups_shield);
    rendertext_draw(fb, txt_col, NULL, text_h, tx, y, buf); y += text_h + 2.0f;
    snprintf(buf, sizeof(buf), "  runs         %d (crash %d, stall %d, sunset %d, quit %d)",
             (int)rs->runs_total, (int)rs->runs_crashed,
             (int)rs->runs_stalled, (int)rs->runs_sunset, (int)rs->runs_quit);
    rendertext_draw(fb, txt_col, NULL, text_h, tx, y, buf); y += text_h + 10.0f;
    return y;
}

static void draw_stats_view(void) {
    // Stats block is the tallest screen — title at 6%, footer at 95%
    // + 14 px → ~98%. Lines are also wider than the other menus so
    // we use a wider panel here.
    draw_menu_panel_size(0.92f, 0.98f);
    float const fbh = pax_buf_get_heightf(fb);
    float const fbw = pax_buf_get_widthf(fb);
    float const tx  = fbw * 0.10f;
    float       y   = fbh * 0.06f;

    draw_centered(y, 32.0f, 0xFFFFFF6Bu, "Stats");
    y += 44.0f;

    y = draw_stats_block(tx, y, "Last run", &s_save.stats.last_run);
    y = draw_stats_block(tx, y, "All time", &s_save.stats.all_time);

    char buf[64];
    snprintf(buf, sizeof(buf), "Level %d  points %d",
             (int)s_save.meta.level, (int)s_save.meta.points);
    rendertext_draw(fb, 0xFFFFFF6Bu, NULL, 18.0f, tx, y, buf);

    draw_centered(fbh * 0.95f, 14.0f, 0xFFA0A0A8u, "press enter or esc to return");
}

// Pause overlay shown over the frozen game scene (STATE_PAUSED).
static void draw_pause_overlay(void) {
    draw_pause_panel();
    float const fbh = pax_buf_get_heightf(fb);
    float const fbw = pax_buf_get_widthf(fb);

    draw_centered(fbh * 0.30f, 48.0f, 0xFFFFFF6Bu, "PAUSED");

    char const* labels[PAUSE_ENTRY_COUNT] = {
        [PAUSE_ENTRY_RESUME] = "Resume",
        [PAUSE_ENTRY_ABORT]  = "Abort run",
    };

    float const top   = fbh * 0.48f;
    float const row_h = 40.0f;
    for (int i = 0; i < PAUSE_ENTRY_COUNT; i++) {
        bool const sel = (i == s_pause_cursor);
        char line[48];
        snprintf(line, sizeof(line), "%s %s %s",
                 sel ? ">" : " ", labels[i], sel ? "<" : " ");
        pax_col_t col = sel ? 0xFFFFFF6Bu : 0xFFFFFFFFu;
        pax_vec2f sz  = rendertext_size(NULL, 28.0f, line);
        rendertext_draw(fb, col, NULL, 28.0f,
                        (fbw - sz.x) * 0.5f, top + (float)i * row_h, line);
    }

    draw_centered(fbh * 0.74f, 14.0f, 0xFFA0A0A8u,
                  "up / down to choose, enter to confirm, F4 to resume");
}

static void draw_upgrade_stub(void) {
    draw_menu_panel_size(0.60f, 0.76f);
    float const fbh = pax_buf_get_heightf(fb);
    draw_centered(fbh * 0.30f, 48.0f, 0xFFFFFF6Bu, "Upgrade Ship");
    draw_centered(fbh * 0.50f, 22.0f, 0xFFFFFFFFu, "Coming soon!");
    draw_centered(fbh * 0.92f, 14.0f, 0xFFA0A0A8u, "press enter or esc to return");
}

// Top-right HUD during PLAYING and GAME_OVER: score + multiplier.
// Returns the y of the bottom of the lowest line drawn so callers
// can stack v= / sun= readouts below it.
static void draw_score_readout(game_state_t const* g) {
    char        buf[48];
    float const text_h = 18.0f;
    float const fbw    = pax_buf_get_widthf(fb);

    snprintf(buf, sizeof(buf), "score=%lld", (long long)g->score);
    pax_vec2f sz = rendertext_size(NULL, text_h, buf);
    rendertext_draw(fb, 0xFFFFFF6Bu, NULL, text_h,
                    fbw - sz.x - 12.0f, 12.0f, buf);
}

// Top-right HUD slot 1 — stage number. Same upcoming-stage rule as
// the rest-area banner: during a rest area between stages N and
// N+1, w->stage is still N, so we add 1 so the HUD shows the same
// number as the banner above. Green to match the banner.
static void draw_stage_readout(world_state_t const* w) {
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
static void draw_stage_banner(int stage) {
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
// the caller so the same world replays exactly on retry. is_custom
// tags the run for save bookkeeping (custom-seed runs will skip
// meta-progression once Phase 11 lands; for now we just record the
// flag so the stats and last_custom_seed updates are correctly
// scoped).
static void start_run(game_state_t* game, world_state_t* world, uint32_t seed, bool is_custom) {
    game_init(game);
    world_init(world, seed);
    input_set_mode(INPUT_MODE_PLAYING);
    s_run_started_us   = esp_timer_get_time();
    s_run_play_seconds = 0.0;
    s_peak_stage       = (int)world->stage;
    s_run_was_custom   = is_custom ? 1 : 0;
    s_run_seed_used    = (int64_t)seed;
    if (is_custom) {
        s_save.meta.last_custom_seed = (int64_t)seed;
    }

    // Install fresh procedural music seeded from the run seed. The
    // mixer takes ownership; on stop / new run it'll call our
    // shutdown callback which frees the struct. Start the engine
    // hum as a persistent SFX voice.
    music_source_t* music = music_procedural_create(seed);
    audio_mixer_set_music(music);
    sfx_engine_hum_start();
}

// Tear down per-run audio (music + persistent SFX). Idle-drain in
// the mixer mutes the speaker amplifier within ~50 ms.
static void end_run_audio(void) {
    audio_mixer_set_music(NULL);
    sfx_engine_hum_stop();
    sfx_scrape_stop();
    // Any one-shot SFX in flight (a still-decaying crash sound on
    // the same frame the run ends) are allowed to play to their
    // natural end — they'll finish well inside the drain window.
}

// Classify the end-of-run cause and commit the run summary to the
// active slot. Called once on the PLAYING → GAME_OVER transition.
static void commit_run_end(game_state_t const* g, world_state_t const* w, bool head_on) {
    save_end_reason_t reason;
    if (head_on) {
        reason = SAVE_END_CRASH;
    } else if (g->sun_y >= GAME_SUN_SINK_RANGE_PX) {
        reason = SAVE_END_SUNSET;
    } else {
        reason = SAVE_END_STALL;
    }
    int   const peak_stage   = (s_peak_stage > (int)w->stage) ? s_peak_stage : (int)w->stage;
    double const run_seconds = s_run_play_seconds;
    save_commit_run_end(s_active_slot, &s_save, reason, g, peak_stage, run_seconds);
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

    // Load music/SFX enable flags and bring the mixer + I2S up.
    // Idempotent — safe even if a later call repeats it.
    audio_settings_load();
    res = audio_mixer_init();
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "audio_mixer_init failed: %d — audio will be silent", res);
    }

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
    // layout until the next midnight rollover.
    uint32_t const daily_seed = derive_daily_seed();
    // The menu/title floor scrolls with a fake speed so the scene
    // reads as "live" instead of static. The world isn't advanced
    // (no obstacles spawn yet) — start_run() initializes the world
    // fresh each time PLAYING begins.
    float const title_scroll_speed = 6.0f;

    // Persistence setup: mkdir /int/synthracer if missing. Defaults
    // until the user picks a slot.
    save_init();
    save_init_defaults(&s_save);

    app_state_t app_state = APP_STATE_SLOT_SELECT;
    bool        run_end_committed = false;
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

    ESP_LOGI(TAG, "Race the Synth: slot-select up");

    while (1) {
        int64_t const t_loop_start = esp_timer_get_time();

        if (input_drain_events()) {
            // F1 = straight exit to launcher. Per the design: this
            // is a dev-only escape hatch and explicitly does NOT
            // save mid-run — losing progress here is by design. The
            // proper "abort a run" path is the F4 pause menu's
            // Abort entry, which commits a QUIT run before returning
            // to the main menu.
            audio_mixer_shutdown();
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

        // Debug sun-position nudge (Q / A). 10 px per press is
        // ~4% of GAME_SUN_SINK_RANGE_PX — coarse enough to feel
        // each press, fine enough to dial in the "fully behind
        // mountains" threshold.
        int const sun_d = input_consume_sun_delta();
        if (sun_d != 0) {
            game.sun_y += (float)sun_d * 10.0f;
            if (game.sun_y < 0.0f)                     game.sun_y = 0.0f;
            if (game.sun_y > GAME_SUN_SINK_RANGE_PX)   game.sun_y = GAME_SUN_SINK_RANGE_PX;
        }

        bool const pickup_pressed = input_consume_pickup();
        int  const steer          = input_steering();

        // Debug: TAB cuts the current area short and forces the
        // next one to a specific type. Currently hard-wired to
        // dynamic gateway; change the area_type_t argument here to
        // test a different generator. Only acts during PLAYING so
        // a stray TAB on a menu doesn't strand the world in an odd
        // state.
        if (input_consume_force_next_area() && app_state == APP_STATE_PLAYING) {
            world_force_next_area(&world, AREA_TYPE_DYNAMIC_GATEWAY);
        }

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
            // game_after_collide runs sun integration, shadow
            // detection, and speed dynamics. Returns true when the
            // ship has coasted to a halt in shadow — same end-of-run
            // signal as a head-on collision.
            bool const stalled = game_after_collide(&game, &world, dt);
            head_on            = head_on || stalled;
            world_advance(&world, dt, game.ship_speed_z, game.cam_x);
            // Accumulate active play time (excludes paused frames).
            // Used by save_commit_run_end so the duration_s stat
            // doesn't count F4 pauses as gameplay time.
            s_run_play_seconds += (double)dt;

            // ---- Per-frame audio driving ----
            // Engine-hum pitch follows ship speed normalised against
            // the standard base speed; boosts push it past 1.0,
            // shadows pull it back.
            float const base_speed = (game.ship_base_speed_z > 0.1f) ? game.ship_base_speed_z : 1.0f;
            float       speed_norm = game.ship_speed_z / base_speed;
            // Map [0.0, 1.5] to [0.0, 1.0] so even cruise sits in
            // the middle of the pitch range and boosts run hot.
            speed_norm *= (1.0f / 1.5f);
            if (speed_norm < 0.0f) speed_norm = 0.0f;
            if (speed_norm > 1.0f) speed_norm = 1.0f;
            sfx_engine_hum_set_pitch(speed_norm);

            // Scrape is persistent — start/stop on edge transitions
            // so we don't pile up registrations.
            static bool s_scrape_was_on = false;
            bool const  scrape_now      = (game.scrape_left || game.scrape_right);
            if (scrape_now && !s_scrape_was_on) {
                sfx_scrape_start();
            }
            if (scrape_now) {
                // Intensity rises with speed — bigger slams sound
                // meatier than a brushing graze.
                float const intensity = 0.4f + 0.6f * speed_norm;
                sfx_scrape_set_intensity(intensity > 1.0f ? 1.0f : intensity);
            }
            if (!scrape_now && s_scrape_was_on) {
                sfx_scrape_stop();
            }
            s_scrape_was_on = scrape_now;

            // Booster ding — edge flag cleared in game_collide each
            // frame, so this fires exactly once per pickup.
            if (game.just_picked_up_booster) {
                sfx_pickup_ding_play();
            }
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
        // PPA SRM destination Y comes from game.sun_y, which the
        // physics step integrates each frame. In TITLE / GAME_OVER
        // states sun_y is wherever the last run left it (0 at start,
        // frozen at end of run).
        ppa_submit_sun((int)game.sun_y);
        ppa_wait_one();
        ppa_submit_mountains();
        int64_t const t_after_bgkick = esp_timer_get_time();
        // Floor paint is split in three so the obstacle-shadow pass
        // can sit between the floor base and the grid lines:
        //   1. synthwave_step_base   — solid floor rectangle
        //   2. render_shadows        — darker quads where obstacles
        //                              cast shadows
        //   3. synthwave_step_lines  — magenta lane lines + stripes
        //                              on top of both
        // Lines on top of shadows keeps them visible in shadowed
        // regions without per-pixel blend math — much cheaper than
        // detecting per-pixel "am I in a shadow" while drawing the
        // grid.
        bool const is_menu_state = (app_state == APP_STATE_SLOT_SELECT
                                    || app_state == APP_STATE_MENU
                                    || app_state == APP_STATE_SEED_INPUT
                                    || app_state == APP_STATE_STATS_VIEW
                                    || app_state == APP_STATE_UPGRADE_STUB);
        // PAUSED freezes the world but keeps the existing scene
        // visible behind the overlay — same render path as
        // GAME_OVER (obstacles + shadows in their last positions,
        // but no scrolling and no fresh shadows).
        float const floor_scroll = is_menu_state                       ? title_scroll_speed * dt
                                   : (app_state == APP_STATE_PLAYING)  ? game.ship_speed_z * dt
                                                                       : 0.0f;
        float const floor_cam_x      = is_menu_state ? 0.0f : game.cam_x;
        bool  const fully_shadowed   = !is_menu_state
                                       && (game.sun_y >= GAME_SUN_SINK_RANGE_PX);
        synthwave_step_base(fb, fully_shadowed);
        if (!is_menu_state) {
            render_shadows(fb, &world, game.cam_x, game.sun_y);
        }
        // Shadow-under-ship sample. After render_shadows has painted
        // every shadow quad (both kind-dispatched defaults and the
        // custom callbacks the bridge span and friends install) on
        // top of the floor base, and *before* synthwave_step_lines
        // overlays lane lines, the floor pixel directly under the
        // ship's foot is either the floor-base 565 (0x5851) or the
        // shadow 565 (0x284A). A single uint16_t comparison turns
        // that into the in_shadow bit — automatically respecting
        // every object's actual painted shadow with no per-object
        // math. The result feeds *next* frame's game_after_collide
        // (one-frame stale; ship moves ~0.3 u/frame at cruise, so
        // the edge transition lags by an imperceptible 0.33 u).
        // Post-sunset is handled synchronously in game_after_collide
        // since the floor base is painted shadow-coloured anyway.
        if (app_state == APP_STATE_PLAYING) {
            float sx, sy;
            render_project(game.ship_x_world, 0.0f, SHIP_COLLISION_Z_C,
                           game.cam_x, &sx, &sy);
            int lx = (int)sx;
            int ly = (int)sy;
            // Ship's foot at z=SHIP_COLLISION_Z_C (~1.98) projects to
            // sy≈483 — below the 480 px floor. Clamp to the last
            // visible row; that pixel corresponds to z ≈ 2.02, only
            // 0.04 u in front of the ship, well inside any normal
            // shadow's z extent. Without this clamp the in-bounds
            // check rejects every sample and the bit never updates.
            if (ly >= DISPLAY_LOG_H) ly = DISPLAY_LOG_H - 1;
            if (lx >= 0 && lx < DISPLAY_LOG_W && ly >= 0) {
                uint16_t const px            = ((uint16_t*)pax_buf_get_pixels(fb))[direct_565_logical_index(lx, ly)];
                uint16_t const shadow_packed = direct_565_pack(GAME_SHADOW_FLOOR_COLOR,
                                                               fb->reverse_endianness);
                game.in_shadow = (px == shadow_packed);
            }
        }
        synthwave_step_lines(fb, floor_scroll, floor_cam_x);
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
        int const menu_nav       = input_consume_menu_nav();
        bool const menu_esc      = input_consume_menu_cancel();
        bool const menu_bs       = input_consume_backspace();
        bool const pause_toggle  = input_consume_pause_toggle();
        int       typed          = -1;
        bool const typed_d       = input_consume_digit(&typed);

        int64_t t_after_obs = 0;
        switch (app_state) {
            case APP_STATE_SLOT_SELECT: {
                t_after_obs = esp_timer_get_time();
                draw_slot_select();
                draw_exit_hint();
                if (menu_nav != 0) {
                    // UP = -1 (toward index 0), DOWN = +1. menu_nav
                    // here is +1 for UP / -1 for DOWN (mirrors the
                    // speed-delta convention); flip the sign so
                    // the cursor moves the way the labels read.
                    s_slot_cursor -= menu_nav;
                    if (s_slot_cursor < 0)               s_slot_cursor = 0;
                    if (s_slot_cursor >= SAVE_SLOT_COUNT) s_slot_cursor = SAVE_SLOT_COUNT - 1;
                }
                if (pickup_pressed) {
                    s_active_slot = s_slot_cursor;
                    if (save_load_slot(s_active_slot, &s_save) != 0) {
                        // Missing or corrupt — start a fresh profile.
                        save_init_defaults(&s_save);
                    }
                    if (s_save.meta.last_custom_seed > 0) {
                        // The seed is logically a uint32 (max 10
                        // digits), so we clamp into 10 chars + null
                        // before formatting; the static analyser
                        // can't deduce the range from a int64 field.
                        uint32_t v = (uint32_t)(s_save.meta.last_custom_seed & 0xFFFFFFFFu);
                        snprintf(s_seed_buf, sizeof(s_seed_buf), "%u", (unsigned)v);
                        s_seed_len = (int)strnlen(s_seed_buf, sizeof(s_seed_buf) - 1);
                    } else {
                        s_seed_buf[0] = '\0';
                        s_seed_len    = 0;
                    }
                    s_menu_cursor = MENU_ENTRY_DAILY;
                    app_state     = APP_STATE_MENU;
                    input_set_mode(INPUT_MODE_TITLE);
                }
                break;
            }

            case APP_STATE_MENU: {
                t_after_obs = esp_timer_get_time();
                draw_main_menu();
                draw_exit_hint();
                if (menu_nav != 0) {
                    s_menu_cursor -= menu_nav;
                    if (s_menu_cursor < 0)                  s_menu_cursor = 0;
                    if (s_menu_cursor >= MENU_ENTRY_COUNT)   s_menu_cursor = MENU_ENTRY_COUNT - 1;
                }
                if (pickup_pressed) {
                    switch (s_menu_cursor) {
                        case MENU_ENTRY_DAILY:
                            start_run(&game, &world, daily_seed, /*is_custom=*/false);
                            run_end_committed = false;
                            app_state = APP_STATE_PLAYING;
                            break;
                        case MENU_ENTRY_SEEDED:
                            input_set_mode(INPUT_MODE_MENU_SEED);
                            app_state = APP_STATE_SEED_INPUT;
                            break;
                        case MENU_ENTRY_UPGRADE:
                            app_state = APP_STATE_UPGRADE_STUB;
                            break;
                        case MENU_ENTRY_STATS:
                            app_state = APP_STATE_STATS_VIEW;
                            break;
                        case MENU_ENTRY_AUDIO:
                            s_audio_cursor = AUDIO_ENTRY_MUSIC;
                            app_state = APP_STATE_AUDIO_SETTINGS;
                            break;
                        case MENU_ENTRY_EXIT:
                            audio_mixer_shutdown();
                            bsp_device_restart_to_launcher();
                            break;
                    }
                }
                break;
            }

            case APP_STATE_SEED_INPUT: {
                t_after_obs = esp_timer_get_time();
                draw_seed_input();
                draw_exit_hint();
                if (typed_d && s_seed_len < (int)(sizeof(s_seed_buf) - 1)) {
                    s_seed_buf[s_seed_len++] = (char)('0' + typed);
                    s_seed_buf[s_seed_len]   = '\0';
                }
                if (menu_bs && s_seed_len > 0) {
                    s_seed_buf[--s_seed_len] = '\0';
                }
                if (menu_esc) {
                    app_state = APP_STATE_MENU;
                    input_set_mode(INPUT_MODE_TITLE);
                }
                if (pickup_pressed && s_seed_len > 0) {
                    uint64_t v   = strtoull(s_seed_buf, NULL, 10);
                    uint32_t seed = (v == 0) ? 1u : (uint32_t)(v & 0xFFFFFFFFu);
                    start_run(&game, &world, seed, /*is_custom=*/true);
                    run_end_committed = false;
                    app_state = APP_STATE_PLAYING;
                }
                break;
            }

            case APP_STATE_AUDIO_SETTINGS: {
                t_after_obs = esp_timer_get_time();
                draw_audio_settings();
                draw_exit_hint();
                if (menu_nav != 0) {
                    s_audio_cursor -= menu_nav;
                    if (s_audio_cursor < 0)                   s_audio_cursor = 0;
                    if (s_audio_cursor >= AUDIO_ENTRY_COUNT)  s_audio_cursor = AUDIO_ENTRY_COUNT - 1;
                }
                if (pickup_pressed) {
                    if (s_audio_cursor == AUDIO_ENTRY_MUSIC) {
                        audio_settings_set_music_on(!audio_settings_music_on());
                    } else {
                        audio_settings_set_sfx_on(!audio_settings_sfx_on());
                    }
                }
                if (menu_esc) {
                    app_state = APP_STATE_MENU;
                }
                break;
            }

            case APP_STATE_STATS_VIEW: {
                t_after_obs = esp_timer_get_time();
                draw_stats_view();
                draw_exit_hint();
                if (pickup_pressed || menu_esc) {
                    app_state = APP_STATE_MENU;
                }
                break;
            }

            case APP_STATE_UPGRADE_STUB: {
                t_after_obs = esp_timer_get_time();
                draw_upgrade_stub();
                draw_exit_hint();
                if (pickup_pressed || menu_esc) {
                    app_state = APP_STATE_MENU;
                }
                break;
            }

            case APP_STATE_PLAYING: {
                // Shadows are already on the floor (drawn between
                // the floor base and the lines above), so we just
                // need the obstacles on top.
                render_obstacles(fb, &world, game.cam_x);
                t_after_obs = esp_timer_get_time();
                game_draw_ship(fb, &game);
                game_draw_sparks(fb, &game);
                if (world.area.type == AREA_TYPE_REST) {
                    // Rest areas (pre-stage-1 lead-in + between-stage
                    // breathers) show the upcoming stage number.
                    // w->stage is N during the rest that leads into
                    // stage N+1, and 0 during the pre-run rest.
                    draw_stage_banner((int)world.stage + 1);
                }
                draw_exit_hint();
                draw_pause_hint();
                draw_score_readout(&game);
                draw_stage_readout(&world);
                draw_speed_readout(game.ship_speed_z);
                draw_sun_readout(game.sun_y);
                draw_boost_indicator(&game);

                // Track peak stage reached this run.
                if ((int)world.stage > s_peak_stage) s_peak_stage = (int)world.stage;

                if (head_on) {
                    if (!run_end_committed) {
                        commit_run_end(&game, &world, head_on);
                        run_end_committed = true;
                    }
                    // The death sound only plays on the actual
                    // PLAYING → GAME_OVER edge — guarded by
                    // run_end_committed (set above) so retriggering
                    // the same head_on flag next frame does nothing.
                    sfx_crash_play();
                    end_run_audio();
                    // Cheap, non-cryptographic per-run flavour roll:
                    // microseconds-since-boot mod count varies enough
                    // for cosmetic text selection.
                    s_gameover_flavour_idx = (int)((uint32_t)esp_timer_get_time() % (uint32_t)GAMEOVER_FLAVOUR_COUNT);
                    app_state = APP_STATE_GAME_OVER;
                    input_set_mode(INPUT_MODE_GAME_OVER);
                } else if (pause_toggle) {
                    s_pause_cursor = PAUSE_ENTRY_RESUME;
                    app_state      = APP_STATE_PAUSED;
                    input_set_mode(INPUT_MODE_PAUSED);
                }
                break;
            }

            case APP_STATE_PAUSED: {
                // Render the world frozen behind the overlay (same
                // approach as GAME_OVER — obstacles + ship in their
                // last positions). The physics step above is gated
                // on PLAYING so nothing moves.
                render_obstacles(fb, &world, game.cam_x);
                t_after_obs = esp_timer_get_time();
                game_draw_ship(fb, &game);
                if (world.area.type == AREA_TYPE_REST) {
                    draw_stage_banner((int)world.stage + 1);
                }
                draw_score_readout(&game);
                draw_stage_readout(&world);
                draw_sun_readout(game.sun_y);
                draw_pause_overlay();
                draw_exit_hint();

                if (menu_nav != 0) {
                    s_pause_cursor -= menu_nav;
                    if (s_pause_cursor < 0)                 s_pause_cursor = 0;
                    if (s_pause_cursor >= PAUSE_ENTRY_COUNT) s_pause_cursor = PAUSE_ENTRY_COUNT - 1;
                }
                if (pause_toggle) {
                    // F4 inside the pause overlay = Resume (matches
                    // the prompt at the bottom of the overlay).
                    app_state = APP_STATE_PLAYING;
                    input_set_mode(INPUT_MODE_PLAYING);
                } else if (pickup_pressed) {
                    if (s_pause_cursor == PAUSE_ENTRY_RESUME) {
                        app_state = APP_STATE_PLAYING;
                        input_set_mode(INPUT_MODE_PLAYING);
                    } else { // PAUSE_ENTRY_ABORT
                        if (!run_end_committed) {
                            int    const peak_stage  = (s_peak_stage > (int)world.stage)
                                                         ? s_peak_stage : (int)world.stage;
                            double const run_seconds = s_run_play_seconds;
                            save_commit_run_end(s_active_slot, &s_save, SAVE_END_QUIT,
                                                &game, peak_stage, run_seconds);
                            run_end_committed = true;
                        }
                        end_run_audio();
                        app_state = APP_STATE_MENU;
                        input_set_mode(INPUT_MODE_TITLE);
                    }
                }
                break;
            }

            case APP_STATE_GAME_OVER: {
                // World frozen at the crash. No sparks here — they
                // are a per-frame radial flash that only reads as
                // a scrape indication during PLAYING. Sun readout
                // stays visible so Q/A nudging still works for
                // visually tuning the sunset threshold.
                render_obstacles(fb, &world, game.cam_x);
                t_after_obs = esp_timer_get_time();
                game_draw_ship(fb, &game);
                if (world.area.type == AREA_TYPE_REST) {
                    draw_stage_banner((int)world.stage + 1);
                }
                draw_game_over_overlay();
                draw_exit_hint();
                draw_score_readout(&game);
                draw_stage_readout(&world);
                draw_sun_readout(game.sun_y);

                if (pickup_pressed) {
                    app_state = APP_STATE_MENU;
                    input_set_mode(INPUT_MODE_TITLE);
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
