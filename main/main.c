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
#include "bsp/input.h"
#include "se_direct565.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "attachments.h"
#include "se_audio.h"
#include "audio_settings.h"
#include "controls_settings.h"
#include "game.h"
#include "hal/lcd_types.h"
#include "hw_settings.h"
#include "icons.h"
#include "input.h"
#include "magicnumbers.h"
#include "se_music_procedural.h"
#include "nvs_flash.h"
#include "pax_gfx.h"
#include "render.h"
#include "se_text.h"
#include "save.h"
#include "scene.h"
#include "sfx/sfx_crash.h"
#include "sfx/sfx_engine_hum.h"
#include "sfx/sfx_gong.h"
#include "sfx/sfx_pickup_ding.h"
#include "sfx/sfx_pickup_plink.h"
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
    APP_STATE_MENU,             // main menu: Daily/Seeded/Upgrade/Stats/Settings/Exit
    APP_STATE_SEED_INPUT,       // numeric entry for the custom seed
    APP_STATE_STATS_VIEW,       // text dump of the active slot's stats
    APP_STATE_UPGRADE,          // equip screen: slot list, shows what's fitted
    APP_STATE_UPGRADE_PICK,     // attachment picker for the selected slot
    APP_STATE_CREDITS,          // auto-scrolling credits roll
    APP_STATE_SETTINGS,         // settings submenu: Controls / Audio
    APP_STATE_CONTROLS,         // controls list: gyro checkbox + 4 keybinds
    APP_STATE_KEY_CAPTURE,      // "press a key" modal for a keybind remap
    APP_STATE_AUDIO_SETTINGS,   // three-checkbox panel: music / SFX / hum
    APP_STATE_PLAYING,
    APP_STATE_PAUSED,           // pause overlay: Resume / Abort run
    APP_STATE_CRASHING,         // post-crash: ship → spark shower, world still flows
    APP_STATE_STALL_OUT,        // post-stall: frozen scene held a beat before GAME_OVER
    APP_STATE_GAME_OVER,
    APP_STATE_CHECKPOINT_REDO,  // post-crash with a checkpoint: run rewound, "Re-Do" dialog
} app_state_t;

// Pause-menu entries (STATE_PAUSED).
enum {
    PAUSE_ENTRY_RESUME = 0,
    PAUSE_ENTRY_SETTINGS,
    PAUSE_ENTRY_ABORT,
    PAUSE_ENTRY_COUNT,
};

// Menu entry indices for STATE_MENU. Order is the visible order.
enum {
    MENU_ENTRY_DAILY = 0,
    MENU_ENTRY_SEEDED,
    MENU_ENTRY_UPGRADE,
    MENU_ENTRY_STATS,
    MENU_ENTRY_SETTINGS,
    MENU_ENTRY_CREDITS,
    MENU_ENTRY_EXIT,
    MENU_ENTRY_COUNT,
};

// Settings submenu entries (STATE_SETTINGS).
enum {
    SETTINGS_ENTRY_CONTROLS = 0,
    SETTINGS_ENTRY_AUDIO,
    SETTINGS_ENTRY_COUNT,
};
static int s_settings_cursor = SETTINGS_ENTRY_CONTROLS;

// Where the settings family (Settings / Controls / Audio / key
// capture) returns to on Esc, and which scene renders behind it.
// APP_STATE_MENU when opened from the main menu (synthwave backdrop,
// scrolling menu floor); APP_STATE_PAUSED when opened from the pause
// menu (frozen game scene behind, run stays logically paused).
static app_state_t s_settings_origin = APP_STATE_MENU;

// Controls-menu entries (STATE_CONTROLS). The gyro checkbox first,
// then the four remappable keybinds in CONTROL_KEY_* order so the
// row index past the checkbox maps straight to a controls_key_t.
enum {
    CONTROLS_ENTRY_GYRO = 0,
    CONTROLS_ENTRY_LEFT,
    CONTROLS_ENTRY_RIGHT,
    CONTROLS_ENTRY_ITEM,
    CONTROLS_ENTRY_PAUSE,
    CONTROLS_ENTRY_COUNT,
};
static int s_controls_cursor = CONTROLS_ENTRY_GYRO;

// Which keybind the open "press a key" modal is rebinding. Set when
// entering STATE_KEY_CAPTURE from a keybind row.
static controls_key_t s_capture_target = CONTROL_KEY_LEFT;

// Audio-settings cursor entries (STATE_AUDIO_SETTINGS).
enum {
    AUDIO_ENTRY_MUSIC = 0,
    AUDIO_ENTRY_SFX,
    AUDIO_ENTRY_HUM,
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
static int  s_upgrade_cursor      = 0;     // APP_STATE_UPGRADE: selected slot row
static int  s_upgrade_slot        = 0;     // slot index (0/1) being edited in the picker
static int  s_upgrade_pick_cursor = 0;     // APP_STATE_UPGRADE_PICK: selected attachment
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

// Calendar day for this whole play session, as yyyymmdd, captured
// once at startup by capture_session_date(). Everything day-
// dependent — the daily world seed, the day-rollover check, the
// (future) daily challenges — reads THIS, never the RTC directly,
// so the "current day" can't shift under the player mid-session
// (e.g. playing across midnight). 0 means the RTC was unset at
// boot (year < 2024) — no usable date.
static int64_t s_session_date = 0;

// End-of-run hold timers + cause. `s_run_was_crash` records whether
// the run ended on a head-on crash (true) or a stall/sunset (false)
// so GAME_OVER commits the correct save reason. The two timers
// accumulate per-frame dt while the matching hold state is active.
static bool   s_run_was_crash   = false;
static double s_crash_anim_time = 0.0;
static double s_stall_hold_time = 0.0;

// Debug godmode (toggled with the G key): crash and stall end-of-run
// conditions are suppressed so the run can be slowed down / inspected.
static bool   s_godmode         = false;

// Crash spark shower runs until game_crash_tick reports all sparks
// spent (≈ the crash SFX length); this cap just guards against an
// abnormally long dt stranding the run in CRASHING.
#define CRASH_ANIM_SECONDS  0.70
// After a stall, hold the frozen scene this long so the player
// registers what happened before the game-over panel appears.
#define STALL_HOLD_SECONDS  1.5

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


// ---- Menu / dialog rendering --------------------------------------
//
// All menus and dialogs are left-aligned. The selection highlight is
// colour only (yellow): a selected row never changes font size and
// never shifts position, so nothing jumps as the cursor moves.
#define MENU_COL_TITLE  0xFFFFFF6Bu   // yellow — screen titles
#define MENU_COL_HILITE 0xFFFFFF6Bu   // yellow — selected row
#define MENU_COL_NORMAL 0xFFFFFFFFu   // white  — unselected rows / body
#define MENU_COL_HINT   0xFFA0A0A8u   // grey   — footer hints
#define MENU_COL_SUB    0xFF808088u   // dim    — secondary text

// Left content x for a panel centred at width fraction `w_frac`: the
// panel's left edge plus a fixed text inset.
static float menu_left_x(float w_frac) {
    float const fbw = pax_buf_get_widthf(fb);
    return (fbw - fbw * w_frac) * 0.5f + 28.0f;
}

// Left-aligned text draw — thin wrapper kept for symmetry with the
// rest of the menu code (every menu draws its rows through this).
static void draw_left(float x, float y, float h, pax_col_t color, char const* text) {
    rendertext_draw(fb, color, NULL, h, x, y, text);
}

// Map a scancode to a key icon, or -1 if none exists. icons.c loads
// PNGs for Esc and F1..F6 (see icon_filenames[] in icons.c); those
// keys render as their icon. Every other key falls back to text via
// scancode_name / scancode_glyph. draw_keybind_value also falls back
// to text if the mapped icon failed to load.
static int scancode_icon(uint16_t sc) {
    switch (sc) {
        case BSP_INPUT_SCANCODE_ESC: return ICON_ESC;
        case BSP_INPUT_SCANCODE_F1:  return ICON_F1;
        case BSP_INPUT_SCANCODE_F2:  return ICON_F2;
        case BSP_INPUT_SCANCODE_F3:  return ICON_F3;
        case BSP_INPUT_SCANCODE_F4:  return ICON_F4;
        case BSP_INPUT_SCANCODE_F5:  return ICON_F5;
        case BSP_INPUT_SCANCODE_F6:  return ICON_F6;
        default: return -1;
    }
}

// Word name for a non-printable key (and for the function keys, used
// as the text fallback when an icon is missing). NULL for printable
// keys — caller falls back to the single glyph.
static char const* scancode_name(uint16_t sc) {
    switch (sc) {
        case BSP_INPUT_SCANCODE_ESC:        return "Esc";
        case BSP_INPUT_SCANCODE_SPACE:      return "Space";
        case BSP_INPUT_SCANCODE_ENTER:      return "Enter";
        case BSP_INPUT_SCANCODE_BACKSPACE:  return "Backspace";
        case BSP_INPUT_SCANCODE_TAB:        return "Tab";
        case BSP_INPUT_SCANCODE_CAPSLOCK:   return "CapsLk";
        case BSP_INPUT_SCANCODE_LEFTSHIFT:  return "L-Shift";
        case BSP_INPUT_SCANCODE_RIGHTSHIFT: return "R-Shift";
        case BSP_INPUT_SCANCODE_LEFTCTRL:   return "Ctrl";
        case BSP_INPUT_SCANCODE_LEFTALT:    return "Alt";
        case BSP_INPUT_SCANCODE_FN:         return "Fn";
        case BSP_INPUT_SCANCODE_F1:  return "F1";
        case BSP_INPUT_SCANCODE_F2:  return "F2";
        case BSP_INPUT_SCANCODE_F3:  return "F3";
        case BSP_INPUT_SCANCODE_F4:  return "F4";
        case BSP_INPUT_SCANCODE_F5:  return "F5";
        case BSP_INPUT_SCANCODE_F6:  return "F6";
        case BSP_INPUT_SCANCODE_F7:  return "F7";
        case BSP_INPUT_SCANCODE_F8:  return "F8";
        case BSP_INPUT_SCANCODE_F9:  return "F9";
        case BSP_INPUT_SCANCODE_F10: return "F10";
        case BSP_INPUT_SCANCODE_F11: return "F11";
        case BSP_INPUT_SCANCODE_F12: return "F12";
        default: return NULL;
    }
}

// Single printable glyph for a scancode, or 0 if the key is not a
// plain printable (caller then uses scancode_name / a hex fallback).
static char scancode_glyph(uint16_t sc) {
    switch (sc) {
        case BSP_INPUT_SCANCODE_1: return '1';
        case BSP_INPUT_SCANCODE_2: return '2';
        case BSP_INPUT_SCANCODE_3: return '3';
        case BSP_INPUT_SCANCODE_4: return '4';
        case BSP_INPUT_SCANCODE_5: return '5';
        case BSP_INPUT_SCANCODE_6: return '6';
        case BSP_INPUT_SCANCODE_7: return '7';
        case BSP_INPUT_SCANCODE_8: return '8';
        case BSP_INPUT_SCANCODE_9: return '9';
        case BSP_INPUT_SCANCODE_0: return '0';
        case BSP_INPUT_SCANCODE_A: return 'A';
        case BSP_INPUT_SCANCODE_B: return 'B';
        case BSP_INPUT_SCANCODE_C: return 'C';
        case BSP_INPUT_SCANCODE_D: return 'D';
        case BSP_INPUT_SCANCODE_E: return 'E';
        case BSP_INPUT_SCANCODE_F: return 'F';
        case BSP_INPUT_SCANCODE_G: return 'G';
        case BSP_INPUT_SCANCODE_H: return 'H';
        case BSP_INPUT_SCANCODE_I: return 'I';
        case BSP_INPUT_SCANCODE_J: return 'J';
        case BSP_INPUT_SCANCODE_K: return 'K';
        case BSP_INPUT_SCANCODE_L: return 'L';
        case BSP_INPUT_SCANCODE_M: return 'M';
        case BSP_INPUT_SCANCODE_N: return 'N';
        case BSP_INPUT_SCANCODE_O: return 'O';
        case BSP_INPUT_SCANCODE_P: return 'P';
        case BSP_INPUT_SCANCODE_Q: return 'Q';
        case BSP_INPUT_SCANCODE_R: return 'R';
        case BSP_INPUT_SCANCODE_S: return 'S';
        case BSP_INPUT_SCANCODE_T: return 'T';
        case BSP_INPUT_SCANCODE_U: return 'U';
        case BSP_INPUT_SCANCODE_V: return 'V';
        case BSP_INPUT_SCANCODE_W: return 'W';
        case BSP_INPUT_SCANCODE_X: return 'X';
        case BSP_INPUT_SCANCODE_Y: return 'Y';
        case BSP_INPUT_SCANCODE_Z: return 'Z';
        case BSP_INPUT_SCANCODE_MINUS:      return '-';
        case BSP_INPUT_SCANCODE_EQUAL:      return '=';
        case BSP_INPUT_SCANCODE_LEFTBRACE:  return '[';
        case BSP_INPUT_SCANCODE_RIGHTBRACE: return ']';
        case BSP_INPUT_SCANCODE_SEMICOLON:  return ';';
        case BSP_INPUT_SCANCODE_APOSTROPHE: return '\'';
        case BSP_INPUT_SCANCODE_GRAVE:      return '`';
        case BSP_INPUT_SCANCODE_BACKSLASH:  return '\\';
        case BSP_INPUT_SCANCODE_COMMA:      return ',';
        case BSP_INPUT_SCANCODE_DOT:        return '.';
        case BSP_INPUT_SCANCODE_SLASH:      return '/';
        default: return 0;
    }
}

// Fill `buf` with the text label for a scancode (used when there is
// no icon for it).
static void keybind_text(uint16_t sc, char* buf, size_t n) {
    char const* name = scancode_name(sc);
    if (name) { snprintf(buf, n, "%s", name); return; }
    char const g = scancode_glyph(sc);
    if (g)    { snprintf(buf, n, "%c", g); return; }
    snprintf(buf, n, "Key 0x%02X", (unsigned)sc);
}

// Draw the value side of a keybind row at (x, y): the function-key
// icon when one exists and loaded, otherwise the text label.
static void draw_keybind_value(float x, float y, float text_h, pax_col_t col, uint16_t sc) {
    int const icon = scancode_icon(sc);
    if (icon >= 0 && icons_width((icon_key_t)icon) > 0) {
        int   const iw = icons_width((icon_key_t)icon);
        int   const ih = icons_height((icon_key_t)icon);
        float const iy = y + text_h * 0.5f - (float)ih * 0.5f;
        // The key-hint PNGs are black glyphs on a transparent
        // background — invisible on the dim menu panel. Lay down a
        // white tile first so the icon reads like a physical keycap.
        pax_simple_rect(fb, 0xFFFFFFFFu, x, iy, (float)iw, (float)ih);
        icons_blit(fb, (icon_key_t)icon, x, iy);
        return;
    }
    char buf[24];
    keybind_text(sc, buf, sizeof(buf));
    draw_left(x, y, text_h, col, buf);
}

// ---- Generic list-menu renderer -----------------------------------
//
// Most menus (main, settings, controls, audio, pause) are the same
// shape: a dim panel, a title, an optional subtitle, a column of
// rows, and a footer hint. `menu_draw()` renders all of them from a
// `menu_view_t` description so each menu is just data, not a bespoke
// draw loop.
//
// The selection chevron is painted as a *separate step* from the row
// text: every row's label starts at the same `text_x` whether or not
// it is selected, and the ">" marker is drawn into a fixed-width
// gutter to its left only for the selected row. Selecting a row thus
// changes its colour and shows the chevron, but never moves the text.
#define MENU_TEXT_INSET     28.0f   // panel edge → chevron gutter
#define MENU_CHEVRON_GUTTER 22.0f   // gutter width reserved for ">"
#define MENU_TOP_PAD        40.0f   // panel top → title baseline
#define MENU_FOOTER_PAD     32.0f   // footer baseline → panel bottom
#define MENU_ROW_TEXT_H     28.0f   // row label / value font height

typedef enum {
    MENU_VAL_NONE = 0,   // plain label row
    MENU_VAL_CHECK,      // label + [X] / [ ]
    MENU_VAL_KEYBIND,    // label + function-key icon / key name
    MENU_VAL_TEXT,       // label + free string in the value column
} menu_val_kind_t;

typedef struct {
    char const*     label;
    menu_val_kind_t kind;
    bool            checked;   // MENU_VAL_CHECK
    uint16_t        scancode;  // MENU_VAL_KEYBIND
    char const*     value;     // MENU_VAL_TEXT
} menu_row_t;

typedef struct {
    char const*       title;
    float             title_h;
    char const*       subtitle;   // NULL → no subtitle line
    menu_row_t const* rows;
    int               row_count;
    float             row_h;
    int               cursor;     // selected row index
    char const*       hint;       // footer hint, NULL → none
    float             panel_w;    // panel width  fraction
    float             panel_h;    // panel height fraction
    float             value_dx;   // x offset of the value column
} menu_view_t;

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

// Debug readout — slot 4, directly below `sun=`. Shows the godmode
// toggle state (G key) plus the ship's world position so the run
// can be inspected while flown around with crash/stall disabled.
static void draw_debug_readout(game_state_t const* g) {
    char        buf[48];
    snprintf(buf, sizeof(buf), "god=%s x=%.2f y=%.2f",
             s_godmode ? "ON" : "off", g->ship_x_world, g->ship_y);
    float const   text_h = 18.0f;
    pax_col_t const col  = s_godmode ? 0xFF31FBFBu : 0xFF808080u;
    pax_vec2f     sz     = rendertext_size(NULL, text_h, buf);
    float const   x      = pax_buf_get_widthf(fb) - sz.x - 12.0f;
    rendertext_draw(fb, col, NULL, text_h, x, 12.0f + 4.0f * (text_h + 4.0f), buf);
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

// Jump-charge inventory HUD (Phase 9.1f) — one red diamond per
// stored jump charge, anchored to the bottom-right corner and
// growing leftward. Nothing is drawn at zero charges.
#define JUMP_HUD_COLOR   0xFFFF4848u
static void draw_jump_inventory(game_state_t const* g) {
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
        // extend left. A square rotated 45° — two triangles split
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
static void draw_shield_inventory(game_state_t const* g) {
    if (g->shield_charges <= 0) return;
    float const margin  = 12.0f;
    float const r       = (18.0f * 3.0f) * 0.5f;   // 27 px — matches the diamonds
    float const spacing = 2.0f * r + 6.0f;
    float const fb_w    = pax_buf_get_widthf(fb);
    float const fb_h    = pax_buf_get_heightf(fb);
    float const cy      = fb_h - margin - r - spacing;   // one row up from jumps
    // Pointy-top unit hexagon corners (corner k at 90 + 60*k degrees).
    static float const HX[6] = { 0.0f, -0.866025f, -0.866025f, 0.0f, 0.866025f, 0.866025f };
    static float const HY[6] = { 1.0f,  0.5f,      -0.5f,     -1.0f, -0.5f,      0.5f      };
    for (int i = 0; i < g->shield_charges; i++) {
        float const cx = fb_w - margin - r - (float)i * spacing;
        // Filled hexagon — a 6-triangle fan from the centre.
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
// 3×3 checkerboard square — the held checkpoint — two rows above the
// jump-charge diamonds. The player holds at most one. Nothing is
// drawn when no checkpoint is held.
static void draw_checkpoint_inventory(game_state_t const* g) {
    if (!g->checkpoint_held) return;
    float const margin  = 12.0f;
    float const r       = (18.0f * 3.0f) * 0.5f;   // 27 px — matches the other symbols
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

// Phase 6 multiplier-HUD layout. Constants live up here so the
// F1 / F4 hint y baselines (which sit *below* the panel across
// every state) can reference them. The draw helper itself
// lives further down in the file beside the other HUD draws.
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
// has been removed — F1 still exits, it just isn't advertised.)
static void draw_pause_hint(void) {
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

// Selection chevron, painted into the row's left gutter as a step
// separate from the label so the label never moves (see menu_view_t).
static void draw_chevron(float x, float y, float text_h) {
    draw_left(x, y, text_h, MENU_COL_HILITE, ">");
}

// Render a whole list menu from its description. See menu_view_t.
static void menu_draw(menu_view_t const* m) {
    draw_menu_panel_size(m->panel_w, m->panel_h);
    float const fbw     = pax_buf_get_widthf(fb);
    float const fbh     = pax_buf_get_heightf(fb);
    float const panel_x = (fbw - fbw * m->panel_w) * 0.5f;
    float const panel_y = (fbh - fbh * m->panel_h) * 0.5f;
    float const panel_h = fbh * m->panel_h;

    float const chevron_x = panel_x + MENU_TEXT_INSET;
    float const text_x    = chevron_x + MENU_CHEVRON_GUTTER;
    float const value_x   = text_x + m->value_dx;

    float y = panel_y + MENU_TOP_PAD;
    draw_left(text_x, y, m->title_h, MENU_COL_TITLE, m->title);
    y += m->title_h + 14.0f;
    if (m->subtitle) {
        draw_left(text_x, y, 18.0f, MENU_COL_NORMAL, m->subtitle);
        y += 18.0f + 16.0f;
    } else {
        y += 14.0f;
    }

    for (int i = 0; i < m->row_count; i++) {
        menu_row_t const* r   = &m->rows[i];
        bool const        sel = (i == m->cursor);
        pax_col_t  const  col = sel ? MENU_COL_HILITE : MENU_COL_NORMAL;
        float const       ry  = y + (float)i * m->row_h;
        if (sel) {
            draw_chevron(chevron_x, ry, MENU_ROW_TEXT_H);
        }
        draw_left(text_x, ry, MENU_ROW_TEXT_H, col, r->label);
        switch (r->kind) {
            case MENU_VAL_CHECK:
                draw_left(value_x, ry, MENU_ROW_TEXT_H, col,
                          r->checked ? "[X]" : "[ ]");
                break;
            case MENU_VAL_KEYBIND:
                draw_keybind_value(value_x, ry, MENU_ROW_TEXT_H, col, r->scancode);
                break;
            case MENU_VAL_TEXT:
                if (r->value) {
                    draw_left(value_x, ry, MENU_ROW_TEXT_H, col, r->value);
                }
                break;
            case MENU_VAL_NONE:
            default:
                break;
        }
    }

    if (m->hint) {
        draw_left(text_x, panel_y + panel_h - MENU_FOOTER_PAD, 14.0f,
                  MENU_COL_HINT, m->hint);
    }
}

// Render the depth-buffered 3D scene for a run: clear the z-buffer,
// emit every obstacle and (optionally) the ship, then rasterize the
// deferred wireframe. The backdrop / floor / shadows must already be
// in the framebuffer — they are 2D layers drawn before this.
static void render_run_scene(world_state_t const* w, game_state_t const* g,
                             bool draw_ship) {
    scene_begin(fb);
    render_submit_obstacles(w);
    if (draw_ship) game_submit_ship(g);
    scene_flush();
}

// Render the scene behind a settings screen. Opened from the pause
// menu, the frozen game (obstacles + ship in their last positions)
// shows through — matching the pause overlay. Opened from the main
// menu there is no run, so this is a no-op and the synthwave
// backdrop drawn earlier in the frame stands.
static void draw_settings_scene(world_state_t const* w, game_state_t const* g) {
    if (s_settings_origin != APP_STATE_PAUSED) return;
    render_run_scene(w, g, true);
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

    float const lx = menu_left_x(0.66f);
    draw_left(lx, fbh * 0.30f, 64.0f, 0xFFF71FF1u, "GAME OVER");
    draw_left(lx, fbh * 0.46f, 22.0f, 0xFF31FBFBu, gameover_flavours[s_gameover_flavour_idx]);
    draw_left(lx, fbh * 0.58f, 22.0f, MENU_COL_NORMAL, "press space to retry");
}

// "Re-Do from checkpoint" dialog (Phase 9.3). Drawn over the
// restored, frozen run scene — pause-like, the run has not ended.
// Wider panel than GAME OVER so the two-line quote fits.
static void draw_checkpoint_redo_overlay(void) {
    float const fbw = pax_buf_get_widthf(fb);
    float const fbh = pax_buf_get_heightf(fb);
    int   const pw  = (int)(fbw * 0.78f);
    int   const ph  = (int)(fbh * 0.50f);
    int   const px  = (int)((fbw - (float)pw) * 0.5f);
    int   const py  = (int)(fbh * 0.21f);
    uint16_t* const pixels = (uint16_t*)pax_buf_get_pixels(fb);
    direct_565_dim_rect(pixels, fb->reverse_endianness, px, py, pw, ph);

    float const lx = menu_left_x(0.78f);
    draw_left(lx, fbh * 0.29f, 44.0f, 0xFF31FBFBu, "Re-Do from checkpoint");
    // Churchill — split across two lines to fit the panel width.
    draw_left(lx, fbh * 0.44f, 21.0f, MENU_COL_NORMAL,
              "Success is not final, failure is not fatal:");
    draw_left(lx, fbh * 0.51f, 21.0f, MENU_COL_NORMAL,
              "it is the courage to continue that counts.");
    draw_left(lx, fbh * 0.63f, 22.0f, 0xFFFFFFFFu, "press space to continue");
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
    // Two-line rows (slot title + summary) don't fit the generic
    // list renderer, but the chevron gutter convention is shared:
    // labels always start at text_x, the ">" is painted separately
    // into the gutter, so selecting a slot never shifts its text.
    draw_menu_panel_size(0.80f, 0.94f);
    float const fbh       = pax_buf_get_heightf(fb);
    float const chevron_x = menu_left_x(0.80f);
    float const text_x    = chevron_x + MENU_CHEVRON_GUTTER;
    draw_left(text_x, fbh * 0.12f, 48.0f, MENU_COL_TITLE, "RACE THE SYNTH");
    draw_left(text_x, fbh * 0.22f, 22.0f, MENU_COL_NORMAL, "select save slot");

    float const row_h = 56.0f;
    float const top   = fbh * 0.34f;
    for (int i = 0; i < SAVE_SLOT_COUNT; i++) {
        save_peek_info_t info  = {0};
        int              exist = save_slot_peek(i, &info) == 0;
        bool const       sel   = (i == s_slot_cursor);
        pax_col_t        title_col = sel ? MENU_COL_HILITE : MENU_COL_NORMAL;
        pax_col_t        sub_col   = sel ? MENU_COL_NORMAL : MENU_COL_SUB;

        char title[32];
        snprintf(title, sizeof(title), "Slot %d", i + 1);

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
        if (sel) {
            draw_chevron(chevron_x, y, 24.0f);
        }
        draw_left(text_x, y, 24.0f, title_col, title);
        draw_left(text_x + 16.0f, y + 28.0f, 16.0f, sub_col, sub);
    }

    draw_left(text_x, fbh * 0.92f, 14.0f, MENU_COL_HINT,
              "up / down to choose, enter to confirm, F1 to exit");
}

// Main menu — six entries.
static void draw_main_menu(void) {
    static char const* const labels[MENU_ENTRY_COUNT] = {
        [MENU_ENTRY_DAILY]    = "Daily Run",
        [MENU_ENTRY_SEEDED]   = "Seeded Run",
        [MENU_ENTRY_UPGRADE]  = "Upgrade Ship",
        [MENU_ENTRY_STATS]    = "Stats",
        [MENU_ENTRY_SETTINGS] = "Settings",
        [MENU_ENTRY_CREDITS]  = "Credits",
        [MENU_ENTRY_EXIT]     = "Exit",
    };
    menu_row_t rows[MENU_ENTRY_COUNT] = {0};
    for (int i = 0; i < MENU_ENTRY_COUNT; i++) {
        rows[i].label = labels[i];
        rows[i].kind  = MENU_VAL_NONE;
    }
    char subtitle[32];
    snprintf(subtitle, sizeof(subtitle), "slot %d", s_active_slot + 1);

    menu_view_t const m = {
        .title = "RACE THE SYNTH", .title_h = 48.0f, .subtitle = subtitle,
        .rows = rows, .row_count = MENU_ENTRY_COUNT, .row_h = 38.0f,
        .cursor = s_menu_cursor, .hint = "up / down to choose, enter to confirm",
        .panel_w = 0.80f, .panel_h = 0.94f, .value_dx = 0.0f,
    };
    menu_draw(&m);
}

// Settings submenu — Controls / Audio.
static void draw_settings_menu(void) {
    static char const* const labels[SETTINGS_ENTRY_COUNT] = {
        [SETTINGS_ENTRY_CONTROLS] = "Controls",
        [SETTINGS_ENTRY_AUDIO]    = "Audio",
    };
    menu_row_t rows[SETTINGS_ENTRY_COUNT] = {0};
    for (int i = 0; i < SETTINGS_ENTRY_COUNT; i++) {
        rows[i].label = labels[i];
        rows[i].kind  = MENU_VAL_NONE;
    }
    menu_view_t const m = {
        .title = "Settings", .title_h = 36.0f, .subtitle = NULL,
        .rows = rows, .row_count = SETTINGS_ENTRY_COUNT, .row_h = 44.0f,
        .cursor = s_settings_cursor,
        .hint = "up / down to choose, enter to open, esc to leave",
        .panel_w = 0.60f, .panel_h = 0.70f, .value_dx = 0.0f,
    };
    menu_draw(&m);
}

// Controls screen — gyro checkbox + four remappable keybinds.
static void draw_controls_menu(void) {
    menu_row_t const rows[CONTROLS_ENTRY_COUNT] = {
        [CONTROLS_ENTRY_GYRO]  = { .label = "Gyroscope", .kind = MENU_VAL_CHECK,
                                   .checked = controls_settings_gyro_on() },
        [CONTROLS_ENTRY_LEFT]  = { .label = "Left", .kind = MENU_VAL_KEYBIND,
                                   .scancode = controls_settings_key(CONTROL_KEY_LEFT) },
        [CONTROLS_ENTRY_RIGHT] = { .label = "Right", .kind = MENU_VAL_KEYBIND,
                                   .scancode = controls_settings_key(CONTROL_KEY_RIGHT) },
        [CONTROLS_ENTRY_ITEM]  = { .label = "Use item", .kind = MENU_VAL_KEYBIND,
                                   .scancode = controls_settings_key(CONTROL_KEY_ITEM) },
        [CONTROLS_ENTRY_PAUSE] = { .label = "Pause", .kind = MENU_VAL_KEYBIND,
                                   .scancode = controls_settings_key(CONTROL_KEY_PAUSE) },
    };
    menu_view_t const m = {
        .title = "Controls", .title_h = 36.0f, .subtitle = NULL,
        .rows = rows, .row_count = CONTROLS_ENTRY_COUNT, .row_h = 46.0f,
        .cursor = s_controls_cursor,
        .hint = "up / down to choose, enter to change, esc to leave",
        .panel_w = 0.74f, .panel_h = 0.86f, .value_dx = 250.0f,
    };
    menu_draw(&m);
}

// "Press a key" modal shown while a keybind is being remapped.
static void draw_key_capture(void) {
    draw_menu_panel_size(0.56f, 0.44f);
    float const fbh = pax_buf_get_heightf(fb);
    float const lx  = menu_left_x(0.56f);

    char const* what = "key";
    switch (s_capture_target) {
        case CONTROL_KEY_LEFT:  what = "Left";     break;
        case CONTROL_KEY_RIGHT: what = "Right";    break;
        case CONTROL_KEY_ITEM:  what = "Use item"; break;
        case CONTROL_KEY_PAUSE: what = "Pause";    break;
        default: break;
    }
    char prompt[48];
    snprintf(prompt, sizeof(prompt), "Rebinding: %s", what);

    draw_left(lx, fbh * 0.40f, 36.0f, MENU_COL_TITLE, "Press a key");
    draw_left(lx, fbh * 0.56f, 22.0f, MENU_COL_NORMAL, prompt);
    draw_left(lx, fbh * 0.66f, 14.0f, MENU_COL_HINT,
              "the next key you press becomes the binding");
}

// Audio-settings screen — three checkboxes, toggled with enter / space.
static void draw_audio_settings(void) {
    menu_row_t const rows[AUDIO_ENTRY_COUNT] = {
        [AUDIO_ENTRY_MUSIC] = { .label = "Music", .kind = MENU_VAL_CHECK,
                                .checked = audio_settings_music_on() },
        [AUDIO_ENTRY_SFX]   = { .label = "Sound effects", .kind = MENU_VAL_CHECK,
                                .checked = audio_settings_sfx_on() },
        [AUDIO_ENTRY_HUM]   = { .label = "Engine hum", .kind = MENU_VAL_CHECK,
                                .checked = audio_settings_hum_on() },
    };
    menu_view_t const m = {
        .title = "Audio", .title_h = 36.0f, .subtitle = NULL,
        .rows = rows, .row_count = AUDIO_ENTRY_COUNT, .row_h = 44.0f,
        .cursor = s_audio_cursor,
        .hint = "up / down to choose, enter to toggle, esc to leave",
        .panel_w = 0.60f, .panel_h = 0.70f, .value_dx = 230.0f,
    };
    menu_draw(&m);
}

// Seed-input screen — numeric entry, prefilled from last_custom_seed.
static void draw_seed_input(void) {
    draw_menu_panel_size(0.70f, 0.76f);
    float const fbh = pax_buf_get_heightf(fb);
    float const lx  = menu_left_x(0.70f);
    draw_left(lx, fbh * 0.20f, 36.0f, MENU_COL_TITLE, "Seeded Run");
    draw_left(lx, fbh * 0.34f, 18.0f, MENU_COL_NORMAL, "enter seed (digits 0-9)");

    char display[16];
    if (s_seed_len == 0) {
        snprintf(display, sizeof(display), "_");
    } else {
        snprintf(display, sizeof(display), "%s_", s_seed_buf);
    }
    draw_left(lx, fbh * 0.50f, 48.0f, MENU_COL_TITLE, display);

    draw_left(lx, fbh * 0.78f, 16.0f, MENU_COL_HINT,
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

    draw_left(tx, y, 32.0f, MENU_COL_TITLE, "Stats");
    y += 44.0f;

    y = draw_stats_block(tx, y, "Last run", &s_save.stats.last_run);
    y = draw_stats_block(tx, y, "All time", &s_save.stats.all_time);

    char buf[64];
    snprintf(buf, sizeof(buf), "Level %d  points %d",
             (int)s_save.meta.level, (int)s_save.meta.points);
    rendertext_draw(fb, 0xFFFFFF6Bu, NULL, 18.0f, tx, y, buf);

    draw_left(tx, fbh * 0.95f, 14.0f, MENU_COL_HINT, "press enter or esc to return");
}

// Pause overlay shown over the frozen game scene (STATE_PAUSED).
static void draw_pause_overlay(void) {
    static char const* const labels[PAUSE_ENTRY_COUNT] = {
        [PAUSE_ENTRY_RESUME]   = "Resume",
        [PAUSE_ENTRY_SETTINGS] = "Settings",
        [PAUSE_ENTRY_ABORT]    = "Abort run",
    };
    menu_row_t rows[PAUSE_ENTRY_COUNT] = {0};
    for (int i = 0; i < PAUSE_ENTRY_COUNT; i++) {
        rows[i].label = labels[i];
        rows[i].kind  = MENU_VAL_NONE;
    }
    menu_view_t const m = {
        .title = "PAUSED", .title_h = 48.0f, .subtitle = NULL,
        .rows = rows, .row_count = PAUSE_ENTRY_COUNT, .row_h = 44.0f,
        .cursor = s_pause_cursor,
        .hint = "up / down to choose, enter to confirm, F4 to resume",
        .panel_w = 0.55f, .panel_h = 0.62f, .value_dx = 0.0f,
    };
    menu_draw(&m);
}

// Phase 9.4 equip UI. The ship has `attach_slots` (0/1/2) equip slots,
// stored in the save as attach1 / attach2 (attachment_id_t, 0 = empty).
// upgrade_slot_ptr maps a slot index to its save field.
static int32_t* upgrade_slot_ptr(int slot) {
    return (slot == 0) ? &s_save.meta.attach1 : &s_save.meta.attach2;
}

// Number of usable slots, clamped to the 2 the save struct provides.
static int upgrade_slot_count(void) {
    int n = s_save.meta.attach_slots;
    if (n < 0) n = 0;
    if (n > 2) n = 2;
    return n;
}

// Slot list — one MENU_VAL_TEXT row per slot showing the fitted
// attachment's name (or "[empty]").
static void draw_upgrade_slots(void) {
    int const slots = upgrade_slot_count();
    if (slots == 0) {
        draw_menu_panel_size(0.60f, 0.50f);
        float const fbh = pax_buf_get_heightf(fb);
        float const lx  = menu_left_x(0.60f);
        draw_left(lx, fbh * 0.34f, 40.0f, MENU_COL_TITLE, "Upgrade Ship");
        draw_left(lx, fbh * 0.52f, 20.0f, MENU_COL_NORMAL, "No attachment slots yet.");
        draw_left(lx, fbh * 0.90f, 14.0f, MENU_COL_HINT, "press enter or esc to return");
        return;
    }
    char       labels[2][16];
    menu_row_t rows[2] = {0};
    for (int i = 0; i < slots; i++) {
        snprintf(labels[i], sizeof(labels[i]), "Slot %d", i + 1);
        rows[i].label = labels[i];
        rows[i].kind  = MENU_VAL_TEXT;
        rows[i].value = attachment_name((attachment_id_t)*upgrade_slot_ptr(i));
    }
    menu_view_t const m = {
        .title = "Upgrade Ship", .title_h = 36.0f, .subtitle = NULL,
        .rows = rows, .row_count = slots, .row_h = 46.0f,
        .cursor = s_upgrade_cursor,
        .hint = "up / down to choose a slot, enter to change, esc to leave",
        .panel_w = 0.70f, .panel_h = 0.62f, .value_dx = 180.0f,
    };
    menu_draw(&m);
}

// Attachment picker for the slot being edited (s_upgrade_slot). Lists
// every catalogued attachment plus "[empty]"; the one already fitted in
// the other slot is tagged "(in slot N)" and blocked from selection.
static void draw_upgrade_picker(void) {
    int     const other_slot = (s_upgrade_slot == 0) ? 1 : 0;
    bool    const other_used = (other_slot < upgrade_slot_count());
    int32_t const other_val  = other_used ? *upgrade_slot_ptr(other_slot) : ATTACH_NONE;

    char       annot[ATTACH_ID_COUNT][24];
    menu_row_t rows[ATTACH_ID_COUNT] = {0};
    for (int i = 0; i < ATTACH_ID_COUNT; i++) {
        rows[i].label = attachment_name((attachment_id_t)i);
        rows[i].kind  = MENU_VAL_NONE;
        if (i != ATTACH_NONE && i == other_val) {
            snprintf(annot[i], sizeof(annot[i]), "(in slot %d)", other_slot + 1);
            rows[i].kind  = MENU_VAL_TEXT;
            rows[i].value = annot[i];
        }
    }
    char title[24];   // "Slot " + worst-case int + NUL (value is only 1-2)
    snprintf(title, sizeof(title), "Slot %d", s_upgrade_slot + 1);
    menu_view_t const m = {
        .title = title, .title_h = 36.0f, .subtitle = NULL,
        .rows = rows, .row_count = ATTACH_ID_COUNT, .row_h = 44.0f,
        .cursor = s_upgrade_pick_cursor,
        .hint = "up / down to choose, enter to equip, esc to cancel",
        .panel_w = 0.66f, .panel_h = 0.66f, .value_dx = 170.0f,
    };
    menu_draw(&m);
}

// ---- Credits ------------------------------------------------------
// The credits text is taller than the panel, so it is scrolled by
// hand with UP / DOWN. The Hershey text renderer writes pixels
// directly and ignores pax_clip, so lines that fall outside the
// viewport are culled whole rather than clipped.
#define CREDITS_PANEL_W     0.86f
#define CREDITS_PANEL_H     0.96f
#define CREDITS_LINE_H      24.0f                  // row pitch
#define CREDITS_TEXT_H      18.0f                  // glyph height
#define CREDITS_SCROLL_STEP (CREDITS_LINE_H * 3.0f) // px per UP/DOWN press

static char const* const credits_lines[] = {
    "Race the Synth was inspired by the steam game \"Race the Sun\"",
    "",
    "Project lead:",
    "    Rene Schickbauer",
    "",
    "Ideas for features:",
    "    Rene Schickbauer",
    "    Renze Nicolai",
    "    People in the Tanmatsu Discord",
    "",
    "Coding:",
    "    Rene Schickbauer",
    "    Claude Code",
    "",
    "Music:",
    "    Claude Code",
    "    Random number generator 23",
    "",
    "Sound effects:",
    "    Rene Schickbauer",
    "    Claude Code",
    "",
    "Level Design:",
    "    Rene Schickbauer",
    "    Claude Code",
    "    Random number generator 42",
    "",
    "Synthwave backdrop:",
    "    Renze Nicolai",
    "",
    "Many thanks to:",
    "    Renze Nicolai for the awesome Tanmatsu device",
    "    Team Badge for making the many modules used in this software",
    "    Espressif for making such a wicked Microcontroller",
    "    Anthropic for Claude Code",
    "",
    "** All music in this game is procedurally generated **",
    "",
    "This software is under the MIT license",
    "https://opensource.org/license/mit",
    "",
    "(C) 2026 Rene \"cavac\" Schickbauer",
};
#define CREDITS_LINE_COUNT ((int)(sizeof(credits_lines) / sizeof(credits_lines[0])))

static float s_credits_scroll = 0.0f;   // px scrolled past the first line

// Height of the credits scroll viewport. Shared by the renderer and
// the scroll clamp so they always agree.
static float credits_scroll_h(void) {
    float const fbh        = pax_buf_get_heightf(fb);
    float const panel_y    = (fbh - fbh * CREDITS_PANEL_H) * 0.5f;
    float const panel_h    = fbh * CREDITS_PANEL_H;
    float const scroll_top = panel_y + MENU_TOP_PAD + 36.0f + 16.0f;
    float const scroll_bot = panel_y + panel_h - MENU_FOOTER_PAD - 10.0f;
    return scroll_bot - scroll_top;
}

// Largest useful scroll offset — the last line resting at the bottom
// of the viewport. 0 when the whole roll already fits.
static float credits_max_scroll(void) {
    float const content_h = (float)CREDITS_LINE_COUNT * CREDITS_LINE_H;
    float const max       = content_h - credits_scroll_h();
    return max > 0.0f ? max : 0.0f;
}

static void draw_credits(void) {
    draw_menu_panel_size(CREDITS_PANEL_W, CREDITS_PANEL_H);
    float const fbw        = pax_buf_get_widthf(fb);
    float const fbh        = pax_buf_get_heightf(fb);
    float const panel_x    = (fbw - fbw * CREDITS_PANEL_W) * 0.5f;
    float const panel_y    = (fbh - fbh * CREDITS_PANEL_H) * 0.5f;
    float const panel_h    = fbh * CREDITS_PANEL_H;
    float const text_x     = panel_x + MENU_TEXT_INSET;
    float const scroll_top = panel_y + MENU_TOP_PAD + 36.0f + 16.0f;
    float const scroll_bot = panel_y + panel_h - MENU_FOOTER_PAD - 10.0f;

    draw_left(text_x, panel_y + MENU_TOP_PAD, 36.0f, MENU_COL_TITLE, "Credits");

    for (int i = 0; i < CREDITS_LINE_COUNT; i++) {
        float const ly = scroll_top - s_credits_scroll + (float)i * CREDITS_LINE_H;
        // Cull whole lines outside the viewport — a partially
        // visible line can't be clipped and would spill over the
        // title / footer.
        if (ly < scroll_top) continue;
        if (ly + CREDITS_TEXT_H > scroll_bot) continue;
        char const*  line = credits_lines[i];
        size_t const len  = strlen(line);
        // Section headings end with ':' — tint them yellow.
        pax_col_t const col = (len > 0 && line[len - 1] == ':')
                                  ? MENU_COL_TITLE : MENU_COL_NORMAL;
        draw_left(text_x, ly, CREDITS_TEXT_H, col, line);
    }

    draw_left(text_x, panel_y + panel_h - MENU_FOOTER_PAD, 14.0f, MENU_COL_HINT,
              "up / down to scroll, enter or esc to return");
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

// Top-left HUD: multiplier panel (Phase 6). Opaque dark-grey
// rectangle holding two rows:
//   1. Four small triangles, filled blue for collected and dark
//      grey for empty. Lit count = `pickups_tri % 5` (always
//      0..4 — the 5th Tri ticks the multiplier and resets the
//      row, so the 5th slot never visually holds).
//   2. The current multiplier as `×N`.
// Layout constants are hoisted up the file (near draw_pause_hint)
// so the F4 hint y baseline can reference them.
static void draw_multiplier_panel(game_state_t const* g) {
    // Opaque background fill — uses pax_simple_rect (not direct_565
    // dim) so the panel is fully readable regardless of scenery.
    pax_simple_rect(fb, MUL_PANEL_BG,
                    (float)MUL_PANEL_X, (float)MUL_PANEL_Y,
                    (float)MUL_PANEL_W, (float)MUL_PANEL_H);

    // 4-slot Tri progress row. `pickups_tri` is monotonic per run;
    // `pickups_tri % 5` is the count of slots to light up. The 5th
    // slot is never visually held — picking up the 5th Tri ticks
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

    // Multiplier text on the bottom row. "×N" centred horizontally
    // within the panel.
    char buf[16];
    snprintf(buf, sizeof(buf), "x%d", g->multiplier);
    float const text_h = 24.0f;
    pax_vec2f   sz     = rendertext_size(NULL, text_h, buf);
    float const tx     = (float)MUL_PANEL_X + ((float)MUL_PANEL_W - sz.x) * 0.5f;
    rendertext_draw(fb, 0xFFFFFF6Bu, NULL, text_h, tx, (float)MUL_TEXT_Y, buf);
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

// The "Stage: N" banner is shown only during the *tail* of a rest
// area — the last STAGE_BANNER_LEAD_Z world-z (≈5 s at cruise)
// before the rest ends and the next stage proper begins. Showing it
// for the whole rest popped the banner up the instant the rest
// stretch first appeared on the far horizon, far too early. The
// short pre-stage-1 intro rest is itself only one screen depth, so
// this threshold leaves its "Stage: 1" banner visible end-to-end.
#define STAGE_BANNER_LEAD_Z  100.0f

static bool stage_banner_visible(world_state_t const* w) {
    return w->area.type == AREA_TYPE_REST
        && w->area.length_remaining_z <= STAGE_BANNER_LEAD_Z;
}

// Read the RTC once and cache the calendar day into s_session_date
// as `year*10000 + month*100 + day`. Call exactly once, early in
// app_main, before anything day-dependent runs. If the RTC is unset
// (year < 2024) s_session_date is left at 0 and the day-dependent
// features fall back to their RTC-unset behaviour.
static void capture_session_date(void) {
    time_t    now = time(NULL);
    struct tm lt  = {0};
    localtime_r(&now, &lt);
    int const year = lt.tm_year + 1900;
    if (year < 2024) {
        s_session_date = 0;  // RTC unset — no usable date
        return;
    }
    s_session_date = (int64_t)year * 10000
                   + (int64_t)(lt.tm_mon + 1) * 100
                   + (int64_t)lt.tm_mday;
}

// Build the daily world seed from the cached session date (see
// s_session_date) — `year*10000 + month*100 + day`. Same day →
// same seed → identical world, every run of the session, and the
// world can't reshuffle between runs if the player crosses
// midnight. Falls back to a fixed constant when the RTC was unset
// at boot, so the run is still reproducible within the session.
//
// The custom-seed menu bypasses this function and passes its own
// seed into start_run().
static uint32_t derive_daily_seed(void) {
    if (s_session_date == 0) {
        return 1u;
    }
    uint32_t const seed = (uint32_t)s_session_date;
    return seed ? seed : 1u;
}

// Day-rollover check. The daily challenge-completion flags
// (save_data.daily.daily_done_*) belong to one specific calendar
// day; once the day moves past the save's last_seen_date they are
// stale and must be cleared. Call once on a freshly loaded slot.
// It compares against the cached s_session_date (NOT the RTC), so
// the rollover is decided exactly once per session — it can never
// fire in the middle of a play session if the clock ticks past
// midnight. The reset is in-memory only — the next run-end commit
// persists it, and the check is idempotent across boots, so no
// extra flash write is forced here. A zero session date (RTC unset
// at boot) is a no-op: with no trustworthy date we can't tell the
// day has changed, so the flags are left untouched.
static void save_apply_day_rollover(save_data_t* s) {
    if (s_session_date == 0) return;                   // RTC unset at boot
    if (s->meta.last_seen_date == s_session_date) return;

    s->meta.last_seen_date  = s_session_date;
    s->daily.daily_done_1pt = 0;
    s->daily.daily_done_2pt = 0;
    s->daily.daily_done_3pt = 0;
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

    // Phase 9.4: snapshot the equipped attachments for the run. Read
    // from the save once here so gameplay (magnet pull + ship-region
    // render gating) never touches the save mid-run. has_battery stays
    // off until Phase 9.5 gives the battery an install state.
    game->has_magnet = (s_save.meta.attach1 == ATTACH_MAGNET
                        || s_save.meta.attach2 == ATTACH_MAGNET);
    // Battery (Phase 9.5): an equippable attachment whose capacity is the
    // meta.battery_max_charge upgrade (0 = no capacity). The battery is
    // active only when it occupies a slot AND has capacity; it starts
    // each run full.
    bool const battery_equipped = (s_save.meta.attach1 == ATTACH_BATTERY
                                   || s_save.meta.attach2 == ATTACH_BATTERY);
    game->battery_max    = battery_equipped ? (float)s_save.meta.battery_max_charge : 0.0f;
    game->has_battery    = (game->battery_max > 0.0f);
    game->battery_charge = game->battery_max;

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
    // hum as a persistent SFX voice — but only if the player
    // hasn't muted it via the Audio settings menu. The hum render
    // function also defensively returns silence if the flag is off,
    // but checking here avoids registering the voice at all and
    // keeps the mixer fully idle when both music and the other
    // SFX are quiet.
    ESP_LOGI(TAG, "start_run: seed=%u is_custom=%d — bringing up audio",
             (unsigned)seed, (int)is_custom);
    music_source_t* music = music_procedural_create(seed);
    if (music == NULL) {
        ESP_LOGW(TAG, "music_procedural_create returned NULL — no music this run");
    }
    audio_mixer_set_music(music);
    if (audio_settings_hum_on()) {
        if (!sfx_engine_hum_start()) {
            ESP_LOGW(TAG, "sfx_engine_hum_start failed");
        }
    }
}

// Edge tracker for the scrape SFX, file-scope so the pause-menu
// and end-of-run transitions can reset it after silencing the
// scrape voice. Without that, scrape held over the boundary
// would skip its restart edge on resume.
static bool s_scrape_was_on = false;

// Tear down per-run audio (music + persistent SFX). Idle-drain in
// the mixer mutes the speaker amplifier within ~50 ms.
static void end_run_audio(void) {
    audio_mixer_set_music(NULL);
    sfx_engine_hum_stop();
    sfx_scrape_stop();
    s_scrape_was_on = false;
    // Any one-shot SFX in flight (a still-decaying crash sound on
    // the same frame the run ends) are allowed to play to their
    // natural end — they'll finish well inside the drain window.
}

// PLAYING → PAUSED. Music keeps playing (pause is "still in the
// run" per the audio design), but every SFX voice is silenced so
// the frozen scene also sounds frozen — no engine hum droning,
// no scrape ringing, no in-flight one-shot tails. The per-effect
// stop calls clear the singleton "is running" flags inside
// sfx_engine_hum / sfx_scrape so resume can cleanly restart them;
// audio_mixer_stop_all_voices() catches the one-shot voices that
// don't have a typed stop API (ding, crash, cube_bump).
static void pause_audio_for_pause_menu(void) {
    sfx_engine_hum_stop();
    sfx_scrape_stop();
    audio_mixer_stop_all_voices();
    s_scrape_was_on = false;
}

// PAUSED → PLAYING. Restart the persistent hum if the player has
// it enabled (the audio settings menu isn't reachable from PAUSED
// today, but reading the flag here makes the resume path future-
// proof against that). Scrape restarts naturally on the next
// collision frame via the s_scrape_was_on edge logic; one-shots
// only fire on their own trigger events.
static void resume_audio_from_pause_menu(void) {
    if (audio_settings_hum_on()) {
        sfx_engine_hum_start();
    }
}

// Classify the end-of-run cause and commit the run summary to the
// active slot. Called once on the PLAYING → GAME_OVER transition.
//
// The save itself is a `fastopen("wb")` + NBT serialise + close
// chain that hits flash synchronously — it can stall the main
// loop for hundreds of ms. We deliberately call this *after* the
// state-machine has already switched to GAME_OVER and the audio
// teardown has happened, so the audible / visible feedback the
// player sees on a crash is immediate and the slow flash write
// hides behind the first GAME_OVER frame.
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

    int64_t const t0 = esp_timer_get_time();
    save_commit_run_end(s_active_slot, &s_save, reason, g, peak_stage, run_seconds);
    int64_t const t1 = esp_timer_get_time();
    ESP_LOGI(TAG, "save commit: reason=%d took %lld ms",
             (int)reason, (long long)((t1 - t0) / 1000));
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
    scene_init();
    icons_load();
    input_init();
    input_set_mode(INPUT_MODE_TITLE);

    // Load music/SFX enable flags and bring the mixer + I2S up.
    // Idempotent — safe even if a later call repeats it.
    audio_settings_load();
    // Control settings (gyro flag + remappable keybinds) — input.c
    // reads these, so load before the first frame.
    controls_settings_load();
    // Capture the calendar day once, now, for the whole session.
    // Every day-dependent feature (daily seed, day-rollover,
    // challenges) reads this snapshot — never the RTC live — so the
    // "current day" can't shift mid-session if the player crosses
    // midnight.
    capture_session_date();
    res = audio_mixer_init();
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "audio_mixer_init failed: %d — audio will be silent", res);
    }
    // Push the loaded audio toggles into the engine mixer's output gates.
    // The mixer no longer reads app settings itself (engine has no NVS
    // dependency); the app maps its mute categories onto mixer groups.
    audio_mixer_set_music_enabled(audio_settings_music_on());
    audio_mixer_set_group_enabled(AUDIO_SFX_GROUP_GENERAL, audio_settings_sfx_on());
    audio_mixer_set_group_enabled(AUDIO_SFX_GROUP_HUM, audio_settings_hum_on());
    // Apply launcher-persisted display/keyboard/LED brightness and
    // speaker/headphone volume + initial audio-jack routing. Has to
    // run after audio_mixer_init() because the mixer's own init
    // sets the amplifier from raw jack state (no NVS); hw_settings
    // then overlays the persisted volume on top.
    hw_settings_init();

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

    // Checkpoint run-state snapshot (Phase 9.3). Collecting a
    // checkpoint copies the whole world + game state here; a later
    // head-on crash restores it. In-memory only — not persisted
    // across power-off. world_state_t carries the obstacle pool, the
    // area + stage + RNG state and the wall cursors; game_state_t the
    // ship, scores, stats, sun and inventory — so the pair is a
    // complete, deterministic resume point.
    static world_state_t s_checkpoint_world;
    static game_state_t  s_checkpoint_game;
    static bool          s_checkpoint_valid = false;
    // Swallows the use-button press-edge on the frame the Re-Do
    // dialog opens, so crashing mid-jump (space held) can't instantly
    // dismiss it.
    static bool          s_redo_ignore_pickup = false;

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

        bool  const pickup_pressed = input_consume_pickup();
        float const steer          = input_steering();
        bool        steer_left = false, steer_right = false;
        input_steer_held(&steer_left, &steer_right);

        // Debug: TAB cuts the current area short and forces the
        // next one to a specific type. Currently hard-wired to the
        // simple_platform area; change the area_type_t argument here
        // to test a different generator. Only acts during PLAYING so
        // a stray TAB on a menu doesn't strand the world in an odd
        // state.
        if (input_consume_force_next_area() && app_state == APP_STATE_PLAYING) {
            world_force_next_area(&world, AREA_TYPE_SYNTHENGINE_AD);
        }
        // Debug: G toggles godmode (crash / stall suppressed below).
        if (input_consume_godmode_toggle()) {
            s_godmode = !s_godmode;
        }

        int64_t const t_after_input = esp_timer_get_time();
        // End-of-run signals for this frame, kept separate so the
        // render switch can pick the right post-run state: a head-on
        // crash plays the explosion, a stall just holds the scene.
        bool crashed = false;
        bool stalled = false;

        // Physics pass — only meaningful in PLAYING; the other states
        // record zero physics time so the breakdown stays honest.
        if (app_state == APP_STATE_PLAYING) {
            // 0. Use-item button triggers a jump (Phase 9.1 — still
            //    ungated; the jump-pickup inventory gate lands in
            //    9.1f). game_jump only fires if the ship is grounded;
            //    game_step then integrates the arc.
            // 1. Apply bank + lateral motion using this frame's steer.
            // 2. Collide: push the ship out of side-contact obstacles
            //    and set scrape flags (or return head-on).
            // 3. After-collide work that reads the flags: ramp speed,
            //    emit + advance sparks.
            if (pickup_pressed) {
                game_jump(&game);
            }
            game_step(&game, &world, dt, steer, steer_left, steer_right);
            crashed = game_collide(&game, &world, dt);
            // game_after_collide runs sun integration, shadow
            // detection, and speed dynamics. Returns true when the
            // ship has coasted to a halt in shadow — the stall
            // end-of-run signal.
            stalled = game_after_collide(&game, &world, dt);
            // Debug godmode: keep the run alive regardless. The ship
            // simply coasts through head-on hits and never stalls
            // out, so the world can be slowed down and inspected.
            if (s_godmode) {
                crashed = false;
                stalled = false;
            }

            // Phase 9.2 shield. Tick the invuln window + the SFX/
            // spark debounce, then let a shield turn a head-on crash
            // into a survivable hit.
            if (game.shield_timer > 0.0f) {
                game.shield_timer -= (float)dt;
                if (game.shield_timer < 0.0f) game.shield_timer = 0.0f;
            }
            if (game.shield_hit_cooldown > 0.0f) {
                game.shield_hit_cooldown -= (float)dt;
                if (game.shield_hit_cooldown < 0.0f) game.shield_hit_cooldown = 0.0f;
            }
            if (crashed) {
                // A banked charge opens the invuln window on the
                // crashing frame.
                if (game.shield_timer <= 0.0f && game.shield_charges > 0) {
                    game.shield_charges--;
                    game.shield_timer = GAME_SHIELD_DURATION;
                }
                // While the window is open the run survives; the
                // crash SFX + spark shower still fire (debounced) so
                // each hit reads.
                if (game.shield_timer > 0.0f) {
                    if (game.shield_hit_cooldown <= 0.0f) {
                        sfx_crash_play();
                        game_crash_burst(&game);
                        game.shield_hit_cooldown = GAME_SHIELD_HIT_COOLDOWN;
                    }
                    crashed = false;
                }
            }
            // Age any shielded-hit spark shower (harmless no-op when
            // the pool is empty).
            game_crash_tick(&game, (float)dt);

            world_advance(&world, dt, game.ship_speed_z, game.cam_x);

            // Phase 9.4 magnet: slide nearby pickups toward the ship's
            // path. After world_advance so it acts on this frame's
            // freshly-advanced positions, before collision sees them.
            if (game.has_magnet) {
                world_magnet_pull(&world, game.ship_x_world,
                                  SHIP_BASE_Y + game.ship_y, (float)dt);
            }

            // Phase 9.3 checkpoint rewind. If a head-on crash got
            // here unabsorbed (no shield) and a checkpoint snapshot
            // exists, rewind the *level* — world generation, RNG,
            // ship position, sun, inventory — to the snapshot and open
            // the Re-Do dialog instead of ending the run.
            //
            // The player's run *progress* is NOT rewound: score,
            // distance, multiplier (+ peak) and the per-run pickup
            // tallies carry forward from the crash-time state, so a
            // Re-Do never costs accumulated progress or the
            // meta-progression credit it feeds at run end. (Max stage
            // reached and play-time live in main.c statics — they
            // already survive the struct copy.) The multiplier pair is
            // carried with pickups_tri on purpose: the two are coupled
            // (the multiplier bumps every 5th Tri), so rewinding one
            // without the other would desync the cadence.
            //
            // Done after world_advance so the restored state is the
            // final word for the frame; `crashed` is consumed so the
            // render switch routes to the dialog, not CRASHING.
            if (crashed && s_checkpoint_valid) {
                sfx_crash_play();

                double const kept_distance     = game.distance_traveled;
                double const kept_score        = game.score;
                int    const kept_multiplier   = game.multiplier;
                int    const kept_mult_max     = game.multiplier_max;
                int    const kept_p_boost      = game.pickups_speed_boost;
                int    const kept_p_tri        = game.pickups_tri;
                int    const kept_p_jump       = game.pickups_jump;
                int    const kept_p_shield     = game.pickups_shield;
                int    const kept_p_checkpoint = game.pickups_checkpoint;

                world = s_checkpoint_world;
                game  = s_checkpoint_game;

                game.distance_traveled  = kept_distance;
                game.score              = kept_score;
                game.multiplier         = kept_multiplier;
                game.multiplier_max     = kept_mult_max;
                game.pickups_speed_boost = kept_p_boost;
                game.pickups_tri         = kept_p_tri;
                game.pickups_jump        = kept_p_jump;
                game.pickups_shield      = kept_p_shield;
                game.pickups_checkpoint  = kept_p_checkpoint;

                s_checkpoint_valid   = false;
                s_redo_ignore_pickup = true;
                app_state = APP_STATE_CHECKPOINT_REDO;
                input_set_mode(INPUT_MODE_GAME_OVER);
                crashed = false;
            }
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
            // so we don't pile up registrations. The "was on" flag
            // lives at module scope (s_scrape_was_on, declared
            // near the audio helpers) so the pause-menu transition
            // can reset it after silencing the scrape voice — without
            // that, scrape staying held across the pause boundary
            // would skip its restart edge.
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

            // Checkpoint pickup (Phase 9.3): snapshot the whole run
            // state — this frame, post-advance — so a later head-on
            // crash can rewind here, and play the gong. The edge
            // flag is cleared before the copy so the snapshot itself
            // records it false (otherwise a restore would re-fire).
            if (game.just_picked_up_checkpoint) {
                game.just_picked_up_checkpoint = false;
                s_checkpoint_world = world;
                s_checkpoint_game  = game;
                s_checkpoint_valid = true;
                sfx_gong_play();
            }

            // Tri plink — pitch steps with the in-cycle slot index
            // so the five pickups of a multiplier cycle form a
            // brief ascending pentatonic phrase (C5/D5/E5/G5/A5).
            // The 5th plink resolves on A5 at the exact frame the
            // HUD progress row resets — audio + visual confirm
            // the multiplier bump together.
            if (game.just_picked_up_tri) {
                sfx_pickup_plink_play(game.tri_pickup_slot);
            }
        } else if (app_state == APP_STATE_CRASHING) {
            // Ship is destroyed — no steering, no collision. Keep
            // the world scrolling at the crash-moment speed so the
            // scene still flows, and age the spark shower. Hold here
            // until the sparks burn out (≈ the crash SFX length),
            // then drop into GAME_OVER.
            world_advance(&world, dt, game.ship_speed_z, game.cam_x);
            s_crash_anim_time += (double)dt;
            bool const sparks_live = game_crash_tick(&game, dt);
            if (!sparks_live || s_crash_anim_time >= CRASH_ANIM_SECONDS) {
                app_state = APP_STATE_GAME_OVER;
            }
        } else if (app_state == APP_STATE_STALL_OUT) {
            // Ship has coasted to a halt. Hold the frozen scene a
            // beat so the player registers the stall, then GAME_OVER.
            s_stall_hold_time += (double)dt;
            if (s_stall_hold_time >= STALL_HOLD_SECONDS) {
                app_state = APP_STATE_GAME_OVER;
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
        // The settings family counts as a "menu state" (scrolling
        // menu floor, no shadows) only when it was opened from the
        // main menu. Opened from the pause menu it renders like
        // PAUSED instead — frozen floor, frozen scene behind.
        bool const in_settings_family = (app_state == APP_STATE_SETTINGS
                                         || app_state == APP_STATE_CONTROLS
                                         || app_state == APP_STATE_AUDIO_SETTINGS
                                         || app_state == APP_STATE_KEY_CAPTURE);
        bool const is_menu_state = (app_state == APP_STATE_SLOT_SELECT
                                    || app_state == APP_STATE_MENU
                                    || app_state == APP_STATE_SEED_INPUT
                                    || app_state == APP_STATE_STATS_VIEW
                                    || app_state == APP_STATE_UPGRADE
                                    || app_state == APP_STATE_UPGRADE_PICK
                                    || app_state == APP_STATE_CREDITS
                                    || (in_settings_family
                                        && s_settings_origin != APP_STATE_PAUSED));
        // PAUSED freezes the world but keeps the existing scene
        // visible behind the overlay — same render path as
        // GAME_OVER (obstacles + shadows in their last positions,
        // but no scrolling and no fresh shadows).
        // CRASHING keeps the world advancing (the wreck's momentum),
        // so its floor scrolls like PLAYING; STALL_OUT / PAUSED /
        // GAME_OVER hold the floor still.
        bool const world_is_live = (app_state == APP_STATE_PLAYING
                                    || app_state == APP_STATE_CRASHING);
        float const floor_scroll = is_menu_state    ? title_scroll_speed * dt
                                   : world_is_live  ? game.ship_speed_z * dt
                                                    : 0.0f;
        float const floor_cam_x      = is_menu_state ? 0.0f : game.cam_x;
        // Camera Y follows a fraction of the ship's jump altitude
        // (GAME_CAM_Y_FOLLOW) so the ship stays in frame without the
        // world lurching; menu states sit at the resting height.
        // Publish the camera to the render module now — *before*
        // anything projects (render_shadows, the floor, obstacles
        // and the ship all read render_project's camera global).
        float const cam_y = is_menu_state
                              ? RENDER_CAM_Y
                              : RENDER_CAM_Y + GAME_CAM_Y_FOLLOW * game.ship_y;
        render_set_camera(floor_cam_x, cam_y);
        bool  const fully_shadowed   = !is_menu_state
                                       && (game.sun_y >= GAME_SUN_SINK_RANGE_PX);
        synthwave_step_base(fb, fully_shadowed);
        if (!is_menu_state) {
            render_shadows(fb, &world, game.cam_x, game.sun_y);
        }
        // (Phase 9.1d: the floor-pixel in_shadow sampler that used to
        // live here is gone. game_collide's shadow ray now computes
        // the gameplay shadow flag directly from geometry — no render
        // dependency, no one-frame lag, and it works at any altitude.
        // render_shadows above still paints the floor-shadow quads,
        // purely as a visual cue now.)
        synthwave_step_lines(fb, floor_scroll, floor_cam_x, cam_y);
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
                    // New calendar day → clear yesterday's daily
                    // challenge-completion flags before play begins.
                    save_apply_day_rollover(&s_save);
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
                            s_upgrade_cursor = 0;
                            app_state = APP_STATE_UPGRADE;
                            break;
                        case MENU_ENTRY_STATS:
                            app_state = APP_STATE_STATS_VIEW;
                            break;
                        case MENU_ENTRY_SETTINGS:
                            s_settings_origin = APP_STATE_MENU;
                            s_settings_cursor = SETTINGS_ENTRY_CONTROLS;
                            app_state = APP_STATE_SETTINGS;
                            break;
                        case MENU_ENTRY_CREDITS:
                            s_credits_scroll = 0.0f;
                            app_state = APP_STATE_CREDITS;
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

            case APP_STATE_SETTINGS: {
                draw_settings_scene(&world, &game);
                t_after_obs = esp_timer_get_time();
                draw_settings_menu();
                if (menu_nav != 0) {
                    s_settings_cursor -= menu_nav;
                    if (s_settings_cursor < 0)                     s_settings_cursor = 0;
                    if (s_settings_cursor >= SETTINGS_ENTRY_COUNT)  s_settings_cursor = SETTINGS_ENTRY_COUNT - 1;
                }
                if (pickup_pressed) {
                    switch (s_settings_cursor) {
                        case SETTINGS_ENTRY_CONTROLS:
                            s_controls_cursor = CONTROLS_ENTRY_GYRO;
                            app_state = APP_STATE_CONTROLS;
                            break;
                        case SETTINGS_ENTRY_AUDIO:
                            s_audio_cursor = AUDIO_ENTRY_MUSIC;
                            app_state = APP_STATE_AUDIO_SETTINGS;
                            break;
                    }
                }
                if (menu_esc) {
                    // Back to wherever Settings was opened from — the
                    // main menu or the pause overlay.
                    app_state = s_settings_origin;
                }
                break;
            }

            case APP_STATE_CONTROLS: {
                draw_settings_scene(&world, &game);
                t_after_obs = esp_timer_get_time();
                draw_controls_menu();
                if (menu_nav != 0) {
                    s_controls_cursor -= menu_nav;
                    if (s_controls_cursor < 0)                     s_controls_cursor = 0;
                    if (s_controls_cursor >= CONTROLS_ENTRY_COUNT)  s_controls_cursor = CONTROLS_ENTRY_COUNT - 1;
                }
                if (pickup_pressed) {
                    if (s_controls_cursor == CONTROLS_ENTRY_GYRO) {
                        controls_settings_set_gyro_on(!controls_settings_gyro_on());
                    } else {
                        // Rows past the gyro checkbox map 1:1 onto
                        // controls_key_t — open the remap modal.
                        s_capture_target =
                            (controls_key_t)(s_controls_cursor - CONTROLS_ENTRY_LEFT);
                        input_begin_key_capture();
                        app_state = APP_STATE_KEY_CAPTURE;
                    }
                }
                if (menu_esc) {
                    app_state = APP_STATE_SETTINGS;
                }
                break;
            }

            case APP_STATE_KEY_CAPTURE: {
                // The backdrop (synthwave, or the frozen game when
                // opened from the pause menu) is already in place;
                // just overlay the modal. No exit hint — F1 is
                // captured as a binding here, not an exit key.
                draw_settings_scene(&world, &game);
                t_after_obs = esp_timer_get_time();
                draw_key_capture();
                uint16_t captured = 0;
                if (input_consume_captured_key(&captured)) {
                    controls_settings_set_key(s_capture_target, captured);
                    app_state = APP_STATE_CONTROLS;
                }
                break;
            }

            case APP_STATE_AUDIO_SETTINGS: {
                draw_settings_scene(&world, &game);
                t_after_obs = esp_timer_get_time();
                draw_audio_settings();
                if (menu_nav != 0) {
                    s_audio_cursor -= menu_nav;
                    if (s_audio_cursor < 0)                   s_audio_cursor = 0;
                    if (s_audio_cursor >= AUDIO_ENTRY_COUNT)  s_audio_cursor = AUDIO_ENTRY_COUNT - 1;
                }
                if (pickup_pressed) {
                    switch (s_audio_cursor) {
                        case AUDIO_ENTRY_MUSIC:
                            audio_settings_set_music_on(!audio_settings_music_on());
                            break;
                        case AUDIO_ENTRY_SFX:
                            audio_settings_set_sfx_on(!audio_settings_sfx_on());
                            break;
                        case AUDIO_ENTRY_HUM:
                            audio_settings_set_hum_on(!audio_settings_hum_on());
                            break;
                    }
                }
                if (menu_esc) {
                    app_state = APP_STATE_SETTINGS;
                }
                break;
            }

            case APP_STATE_STATS_VIEW: {
                t_after_obs = esp_timer_get_time();
                draw_stats_view();
                if (pickup_pressed || menu_esc) {
                    app_state = APP_STATE_MENU;
                }
                break;
            }

            case APP_STATE_UPGRADE: {
                t_after_obs = esp_timer_get_time();
                draw_upgrade_slots();
                int const slots = upgrade_slot_count();
                if (menu_nav != 0 && slots > 0) {
                    s_upgrade_cursor -= menu_nav;
                    if (s_upgrade_cursor < 0)        s_upgrade_cursor = 0;
                    if (s_upgrade_cursor >= slots)   s_upgrade_cursor = slots - 1;
                }
                if (pickup_pressed && slots > 0) {
                    // Open the picker for the selected slot, starting on
                    // whatever it currently holds.
                    s_upgrade_slot        = s_upgrade_cursor;
                    s_upgrade_pick_cursor = *upgrade_slot_ptr(s_upgrade_slot);
                    if (s_upgrade_pick_cursor < 0
                        || s_upgrade_pick_cursor >= ATTACH_ID_COUNT) {
                        s_upgrade_pick_cursor = 0;
                    }
                    app_state = APP_STATE_UPGRADE_PICK;
                } else if (pickup_pressed || menu_esc) {
                    // No slots, or esc — back to the main menu.
                    app_state = APP_STATE_MENU;
                }
                break;
            }

            case APP_STATE_UPGRADE_PICK: {
                t_after_obs = esp_timer_get_time();
                draw_upgrade_picker();
                if (menu_nav != 0) {
                    s_upgrade_pick_cursor -= menu_nav;
                    if (s_upgrade_pick_cursor < 0)                 s_upgrade_pick_cursor = 0;
                    if (s_upgrade_pick_cursor >= ATTACH_ID_COUNT)  s_upgrade_pick_cursor = ATTACH_ID_COUNT - 1;
                }
                if (pickup_pressed) {
                    int     const other_slot = (s_upgrade_slot == 0) ? 1 : 0;
                    bool    const other_used = (other_slot < upgrade_slot_count());
                    int32_t const other_val  = other_used ? *upgrade_slot_ptr(other_slot) : ATTACH_NONE;
                    // Block equipping a (non-empty) attachment already in
                    // the other slot — no duplicates. Ignore the press.
                    if (s_upgrade_pick_cursor != ATTACH_NONE
                        && s_upgrade_pick_cursor == other_val) {
                        break;
                    }
                    *upgrade_slot_ptr(s_upgrade_slot) = s_upgrade_pick_cursor;
                    save_write_slot(s_active_slot, &s_save);
                    app_state = APP_STATE_UPGRADE;
                } else if (menu_esc) {
                    app_state = APP_STATE_UPGRADE;
                }
                break;
            }

            case APP_STATE_CREDITS: {
                t_after_obs = esp_timer_get_time();
                if (menu_nav != 0) {
                    // UP (menu_nav +1) scrolls toward the top of the
                    // roll, DOWN (-1) toward the bottom.
                    s_credits_scroll -= (float)menu_nav * CREDITS_SCROLL_STEP;
                    if (s_credits_scroll < 0.0f) s_credits_scroll = 0.0f;
                    float const max = credits_max_scroll();
                    if (s_credits_scroll > max) s_credits_scroll = max;
                }
                draw_credits();
                if (pickup_pressed || menu_esc) {
                    app_state = APP_STATE_MENU;
                }
                break;
            }

            case APP_STATE_PLAYING: {
                // Shadows are already on the floor (drawn between
                // the floor base and the lines above); the depth-
                // buffered scene (obstacles + ship) goes on top.
                render_run_scene(&world, &game, true);
                t_after_obs = esp_timer_get_time();
                game_draw_sparks(fb, &game);
                // Spark shower from a shield-absorbed hit (Phase 9.2)
                // — only live during the invuln window; a no-op
                // otherwise.
                game_draw_crash_sparks(fb, &game);
                // Violet ring around the ship while the shield's
                // invuln window is open.
                game_draw_shield(fb, &game);
                if (stage_banner_visible(&world)) {
                    // Rest areas (pre-stage-1 lead-in + between-stage
                    // breathers) announce the upcoming stage number
                    // in their final stretch. w->stage is N during
                    // the rest that leads into stage N+1, and 0
                    // during the pre-run intro rest.
                    draw_stage_banner((int)world.stage + 1);
                }
                draw_multiplier_panel(&game);
                draw_pause_hint();
                draw_score_readout(&game);
                draw_stage_readout(&world);
                draw_speed_readout(game.ship_speed_z);
                draw_sun_readout(game.sun_y);
                draw_debug_readout(&game);
                draw_boost_indicator(&game);
                draw_jump_inventory(&game);
                draw_shield_inventory(&game);
                draw_checkpoint_inventory(&game);

                // Track peak stage reached this run.
                if ((int)world.stage > s_peak_stage) s_peak_stage = (int)world.stage;

                if (crashed) {
                    // Head-on crash. Fire the explosion SFX first —
                    // the audio task then has a fresh sample to play
                    // through any cache-disable stall — then tear
                    // down the run's persistent voices, spawn the
                    // spark shower in place of the ship, and enter
                    // CRASHING. The world keeps flowing and the
                    // sparks animate there until they burn out
                    // (≈ the crash SFX length), at which point the
                    // physics pass flips to GAME_OVER. The slow
                    // end-of-run flash save stays deferred to
                    // GAME_OVER's first frame.
                    sfx_crash_play();
                    end_run_audio();
                    game_crash_burst(&game);
                    s_run_was_crash   = true;
                    s_crash_anim_time = 0.0;
                    s_gameover_flavour_idx = (int)((uint32_t)esp_timer_get_time() % (uint32_t)GAMEOVER_FLAVOUR_COUNT);
                    app_state = APP_STATE_CRASHING;
                    input_set_mode(INPUT_MODE_GAME_OVER);
                } else if (stalled) {
                    // Ship coasted to a halt (shadow stall / sunset).
                    // No explosion SFX — just stop the run audio and
                    // hold the frozen scene a beat in STALL_OUT so
                    // the player registers the stall before the
                    // game-over panel appears.
                    end_run_audio();
                    s_run_was_crash   = false;
                    s_stall_hold_time = 0.0;
                    s_gameover_flavour_idx = (int)((uint32_t)esp_timer_get_time() % (uint32_t)GAMEOVER_FLAVOUR_COUNT);
                    app_state = APP_STATE_STALL_OUT;
                    input_set_mode(INPUT_MODE_GAME_OVER);
                } else if (pause_toggle) {
                    s_pause_cursor = PAUSE_ENTRY_RESUME;
                    app_state      = APP_STATE_PAUSED;
                    input_set_mode(INPUT_MODE_PAUSED);
                    pause_audio_for_pause_menu();
                }
                break;
            }

            case APP_STATE_CRASHING: {
                // The world still flows (physics advanced it above);
                // the ship is gone, replaced by the spark shower.
                // No pause hint, no input — the physics pass drops
                // us into GAME_OVER once the sparks burn out.
                render_run_scene(&world, &game, false);
                t_after_obs = esp_timer_get_time();
                game_draw_crash_sparks(fb, &game);
                if (stage_banner_visible(&world)) {
                    draw_stage_banner((int)world.stage + 1);
                }
                draw_multiplier_panel(&game);
                draw_score_readout(&game);
                draw_stage_readout(&world);
                draw_sun_readout(game.sun_y);
                break;
            }

            case APP_STATE_STALL_OUT: {
                // Frozen scene held for a beat after the stall. The
                // ship is still drawn — it sat down, it didn't blow
                // up. No input; the physics pass times out into
                // GAME_OVER.
                render_run_scene(&world, &game, true);
                t_after_obs = esp_timer_get_time();
                if (stage_banner_visible(&world)) {
                    draw_stage_banner((int)world.stage + 1);
                }
                draw_multiplier_panel(&game);
                draw_score_readout(&game);
                draw_stage_readout(&world);
                draw_sun_readout(game.sun_y);
                break;
            }

            case APP_STATE_PAUSED: {
                // Render the world frozen behind the overlay (same
                // approach as GAME_OVER — obstacles + ship in their
                // last positions). The physics step above is gated
                // on PLAYING so nothing moves.
                render_run_scene(&world, &game, true);
                t_after_obs = esp_timer_get_time();
                if (stage_banner_visible(&world)) {
                    draw_stage_banner((int)world.stage + 1);
                }
                draw_multiplier_panel(&game);
                draw_score_readout(&game);
                draw_stage_readout(&world);
                draw_sun_readout(game.sun_y);
                draw_pause_overlay();

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
                    resume_audio_from_pause_menu();
                } else if (pickup_pressed) {
                    switch (s_pause_cursor) {
                        case PAUSE_ENTRY_RESUME:
                            app_state = APP_STATE_PLAYING;
                            input_set_mode(INPUT_MODE_PLAYING);
                            resume_audio_from_pause_menu();
                            break;
                        case PAUSE_ENTRY_SETTINGS:
                            // Open Settings over the frozen run. The
                            // run stays logically paused — no audio
                            // resume — and Esc walks back here.
                            s_settings_origin = APP_STATE_PAUSED;
                            s_settings_cursor = SETTINGS_ENTRY_CONTROLS;
                            app_state = APP_STATE_SETTINGS;
                            break;
                        case PAUSE_ENTRY_ABORT:
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
                            break;
                    }
                }
                break;
            }

            case APP_STATE_GAME_OVER: {
                // Deferred from the CRASHING / STALL_OUT hold states
                // so the slow flash write happens after the audio
                // teardown and the post-run animation. Runs once on
                // the first GAME_OVER frame; `run_end_committed`
                // keeps it from re-firing. `s_run_was_crash` carries
                // the real end cause (head-on crash vs stall/sunset)
                // so the save records the correct reason.
                if (!run_end_committed) {
                    commit_run_end(&game, &world, s_run_was_crash);
                    run_end_committed = true;
                }

                // World frozen at the end of the run. Sun readout
                // stays visible so Q/A nudging still works for
                // visually tuning the sunset threshold.
                // A crashed ship was blown to sparks during CRASHING
                // — don't resurrect it under the panel. A stalled
                // ship is still sitting on the track, so draw it.
                render_run_scene(&world, &game, !s_run_was_crash);
                t_after_obs = esp_timer_get_time();
                if (stage_banner_visible(&world)) {
                    draw_stage_banner((int)world.stage + 1);
                }
                draw_game_over_overlay();
                draw_multiplier_panel(&game);
                draw_score_readout(&game);
                draw_stage_readout(&world);
                draw_sun_readout(game.sun_y);

                if (pickup_pressed) {
                    app_state = APP_STATE_MENU;
                    input_set_mode(INPUT_MODE_TITLE);
                }
                break;
            }

            case APP_STATE_CHECKPOINT_REDO: {
                // The run has been rewound to the checkpoint snapshot
                // (done in the physics pass). This is a pause-like
                // hold — the restored scene is frozen behind the
                // dialog (physics is gated on PLAYING), music keeps
                // playing, the run is NOT committed. Space resumes.
                render_run_scene(&world, &game, true);
                t_after_obs = esp_timer_get_time();
                if (stage_banner_visible(&world)) {
                    draw_stage_banner((int)world.stage + 1);
                }
                draw_multiplier_panel(&game);
                draw_score_readout(&game);
                draw_stage_readout(&world);
                draw_sun_readout(game.sun_y);
                draw_checkpoint_redo_overlay();

                if (pickup_pressed && !s_redo_ignore_pickup) {
                    // Resume: spend the checkpoint and grant the
                    // shield's invulnerability window so the player
                    // gets a moment of grace on the way back in.
                    game.checkpoint_held = false;
                    game.shield_timer    = GAME_SHIELD_DURATION;
                    app_state = APP_STATE_PLAYING;
                    input_set_mode(INPUT_MODE_PLAYING);
                }
                s_redo_ignore_pickup = false;
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
