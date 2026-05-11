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

// PPA SRM client + completion semaphore for the async per-frame
// backdrop copy. The PPA op copies just the *above-horizon* region
// of `backdrop_cache` (sky + sun + mountains + wireframe + top
// horizon line) into the framebuffer, in parallel with the CPU
// painting the floor area in `synthwave_step`. The semaphore is
// given by the PPA "done" callback and taken before any code that
// touches the sky region (currently `render_obstacles`, since
// obstacles can extend above the horizon).
static ppa_client_handle_t          ppa_client     = NULL;
static SemaphoreHandle_t            ppa_done_sem   = NULL;
static bool                         ppa_pending    = false;

// ESP32-P4 PSRAM L2 cache line size. Used both for the aligned
// allocation of `backdrop_cache` and for `esp_cache_msync` operations.
#define PPA_PSRAM_CACHE_LINE 128

// Horizon row in logical coordinates. `synthwave_draw_top_grid`
// paints a magenta line at this y; `synthwave_step` starts the
// floor rect at y = HORIZON_LOGICAL_Y + 1. PPA copies logical rows
// [0, HORIZON_LOGICAL_Y] inclusive, so the horizon line itself is
// carried over from the cached backdrop.
#define HORIZON_LOGICAL_Y 256

static char const TAG[] = "racethesynth";

static size_t                       display_h_res        = 0;
static size_t                       display_v_res        = 0;
static lcd_color_rgb_pixel_format_t display_color_format = LCD_COLOR_PIXEL_FORMAT_RGB888;
static lcd_rgb_data_endian_t        display_data_endian  = LCD_RGB_DATA_ENDIAN_LITTLE;
static pax_buf_t                    fb                   = {0};
// Pre-rendered static synthwave layers (sky, sun, mountains, wireframe,
// top horizon line). Copied wholesale into `fb` at the start of each
// frame so the per-frame work skips the expensive triangulated polygon
// fills + ~200 wireframe lines and only needs to redraw the dynamic
// content (scrolling grid floor, ship, HUD).
static pax_buf_t                    backdrop_cache       = {0};
static void*                        backdrop_pixels      = NULL;
static size_t                       backdrop_size        = 0;

typedef enum {
    APP_STATE_TITLE = 0,
    APP_STATE_PLAYING,
    APP_STATE_GAME_OVER,
} app_state_t;

static void blit(void) {
    bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&fb));
}

// PPA "transaction done" callback. Runs in interrupt context; the
// only thing we do here is give the semaphore so the main loop can
// proceed past `bgwait`. Return value tells the PPA driver whether
// a higher-priority task became ready (yes if xSemaphoreGiveFromISR
// woke one).
static bool ppa_on_trans_done(ppa_client_handle_t client, ppa_event_data_t *event_data, void *user_data) {
    (void)client;
    (void)event_data;
    (void)user_data;
    BaseType_t hpw = pdFALSE;
    xSemaphoreGiveFromISR(ppa_done_sem, &hpw);
    return hpw == pdTRUE;
}

// Submit the per-frame async backdrop copy. The source rect covers
// logical rows [0, HORIZON_LOGICAL_Y] of `backdrop_cache`; the
// destination rect is the matching region of `fb`. Returns true on
// success — caller must call `ppa_wait_backdrop` exactly once per
// successful submit before the floor or obstacle code touches the
// sky region of `fb`.
//
// Orientation: the Tanmatsu LCD reports BSP_DISPLAY_ROTATION_270
// (PAX orientation `PAX_O_ROT_CW`), so raw_w = display_h_res = 480
// and the logical-y → raw-x mapping is `rx = raw_w - 1 - ly`. The
// above-horizon strip therefore lives at raw columns
// [raw_w - sky_rows, raw_w - 1] = [223, 479] = 257 columns, with
// full raw_h = 800 rows. PPA SRM operates on the raw buffer.
static bool ppa_submit_backdrop(void) {
    int const sky_rows   = HORIZON_LOGICAL_Y + 1;        // 257 logical rows
    int const raw_w      = (int)display_h_res;
    int const raw_h      = (int)display_v_res;
    int const rx_start   = raw_w - sky_rows;             // 480 - 257 = 223
    if (rx_start < 0) return false;

    ppa_srm_oper_config_t cfg = {
        .in = {
            .buffer         = backdrop_pixels,
            .pic_w          = (uint32_t)raw_w,
            .pic_h          = (uint32_t)raw_h,
            .block_w        = (uint32_t)sky_rows,
            .block_h        = (uint32_t)raw_h,
            .block_offset_x = (uint32_t)rx_start,
            .block_offset_y = 0,
            .srm_cm         = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer         = (void*)pax_buf_get_pixels(&fb),
            .buffer_size    = backdrop_size,
            .pic_w          = (uint32_t)raw_w,
            .pic_h          = (uint32_t)raw_h,
            .block_offset_x = (uint32_t)rx_start,
            .block_offset_y = 0,
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
    esp_err_t err = ppa_do_scale_rotate_mirror(ppa_client, &cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ppa_do_scale_rotate_mirror failed: %d", err);
        return false;
    }
    ppa_pending = true;
    return true;
}

// Take the completion semaphore. Returns true once the PPA op has
// finished (or was never submitted this frame). 50 ms timeout is
// generous — PPA SRM of 257×800 RGB565 should complete in well
// under 5 ms.
static void ppa_wait_backdrop(void) {
    if (!ppa_pending) return;
    if (xSemaphoreTake(ppa_done_sem, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "PPA backdrop wait timed out");
    }
    ppa_pending = false;
}

static void draw_centered(float cy, float h, pax_col_t color, char const* text) {
    pax_vec2f sz = rendertext_size(NULL, h, text);
    float const x = (pax_buf_get_widthf(&fb) - sz.x) * 0.5f;
    rendertext_draw(&fb, color, NULL, h, x, cy, text);
}

static void draw_speed_readout(float speed_z) {
    char        buf[32];
    snprintf(buf, sizeof(buf), "v=%.1f", speed_z);
    float const text_h = 18.0f;
    pax_vec2f   sz     = rendertext_size(NULL, text_h, buf);
    float const x      = pax_buf_get_widthf(&fb) - sz.x - 12.0f;
    rendertext_draw(&fb, 0xFFFFFFFF, NULL, text_h, x, 12.0f, buf);
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
        icons_blit(&fb, ICON_F1, x_margin, icon_y);
        rendertext_draw(&fb, 0xFFFFFFFF, NULL, prompt_h, text_x, y, prompt);
    } else {
        char const* fallback = "F1 to exit";
        rendertext_draw(&fb, 0xFFFFFFFF, NULL, prompt_h, x_margin, y, fallback);
    }
}

static void draw_title_overlay(void) {
    float const fbh = pax_buf_get_heightf(&fb);
    draw_centered(fbh * 0.30f, 64.0f, 0xFFFFFF6Bu, "RACE THE SYNTH");
    draw_centered(fbh * 0.52f, 22.0f, 0xFFFFFFFFu, "press space to start");
}

static void draw_game_over_overlay(void) {
    float const fbh = pax_buf_get_heightf(&fb);
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

    pax_buf_init(&fb, NULL, display_h_res, display_v_res, format);
    pax_buf_reversed(&fb, display_data_endian == LCD_RGB_DATA_ENDIAN_BIG);
    pax_buf_set_orientation(&fb, orientation);

    synthwave_init();
    icons_load();
    input_init();
    input_set_mode(INPUT_MODE_TITLE);

    backdrop_size   = pax_buf_get_size(&fb);
    // Cache-line-aligned PSRAM so PPA can DMA-read directly. On
    // ESP32-P4 PSRAM is AXI-DMA-accessible without MALLOC_CAP_DMA
    // (which historically maps to AHB-DMA-able internal SRAM and
    // doesn't combine cleanly with MALLOC_CAP_SPIRAM). The PPA
    // driver requires 128-byte alignment on external memory; size
    // (768000 bytes at 480×800 RGB565) is already a multiple of 128
    // so no padding is needed.
    backdrop_pixels = heap_caps_aligned_alloc(PPA_PSRAM_CACHE_LINE, backdrop_size,
                                              MALLOC_CAP_SPIRAM);
    if (backdrop_pixels == NULL) {
        ESP_LOGE(TAG, "Failed to allocate %u bytes for backdrop cache", (unsigned)backdrop_size);
        return;
    }
    pax_buf_init(&backdrop_cache, backdrop_pixels, display_h_res, display_v_res, format);
    pax_buf_reversed(&backdrop_cache, display_data_endian == LCD_RGB_DATA_ENDIAN_BIG);
    pax_buf_set_orientation(&backdrop_cache, orientation);

    synthwave_draw_sky(&backdrop_cache);
    synthwave_draw_sun(&backdrop_cache, 0.0f);
    synthwave_draw_mountains(&backdrop_cache);
    synthwave_draw_wireframe(&backdrop_cache);
    synthwave_draw_top_grid(&backdrop_cache);

    // Backdrop is now fully rasterised by PAX into CPU cache; flush
    // it to PSRAM so the PPA DMA engine sees the finished pixels.
    // One-shot — the backdrop is never modified again, so subsequent
    // frames need no further cache maintenance for the source.
    esp_err_t cache_err = esp_cache_msync(backdrop_pixels, backdrop_size,
                                          ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                                          ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    if (cache_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_cache_msync(backdrop) failed: %d", cache_err);
        return;
    }

    // PPA SRM client + per-frame completion semaphore. One pending
    // transaction is enough because the main loop always waits on
    // the previous frame's op before submitting the next.
    ppa_done_sem = xSemaphoreCreateBinary();
    if (ppa_done_sem == NULL) {
        ESP_LOGE(TAG, "Failed to create PPA semaphore");
        return;
    }
    ppa_client_config_t const ppa_cfg = {
        .oper_type             = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
        .data_burst_length     = PPA_DATA_BURST_LENGTH_128,
    };
    esp_err_t ppa_err = ppa_register_client(&ppa_cfg, &ppa_client);
    if (ppa_err != ESP_OK) {
        ESP_LOGE(TAG, "ppa_register_client failed: %d", ppa_err);
        return;
    }
    ppa_event_callbacks_t const ppa_cbs = {
        .on_trans_done = ppa_on_trans_done,
    };
    ppa_err = ppa_client_register_event_callbacks(ppa_client, &ppa_cbs);
    if (ppa_err != ESP_OK) {
        ESP_LOGE(TAG, "ppa_client_register_event_callbacks failed: %d", ppa_err);
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

        // Background pass — kick the PPA SRM copy of the above-
        // horizon region (sky/sun/mountains/wireframe/top-grid)
        // asynchronously, then paint the floor area on the CPU
        // while PPA runs in parallel. The PPA writes the upper
        // rows of `fb` directly to PSRAM; CPU writes the floor
        // rows. The two regions don't overlap, so no write race.
        // Same work in every state.
        ppa_submit_backdrop();
        int64_t const t_after_bgkick = esp_timer_get_time();
        float const floor_scroll = (app_state == APP_STATE_TITLE)      ? title_scroll_speed * dt
                                   : (app_state == APP_STATE_PLAYING)  ? game.ship_speed_z * dt
                                                                       : 0.0f;
        float const floor_cam_x  = (app_state == APP_STATE_TITLE) ? 0.0f : game.cam_x;
        synthwave_step(&fb, floor_scroll, floor_cam_x);
        int64_t const t_after_bgflr = esp_timer_get_time();
        // Block until PPA finishes — obstacles and HUD text can
        // both write into the sky region, so the backdrop must be
        // in place before any foreground rendering touches it.
        ppa_wait_backdrop();
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
                render_obstacles(&fb, &world, game.cam_x);
                t_after_obs = esp_timer_get_time();
                game_draw_ship(&fb, &game);
                game_draw_sparks(&fb, &game);
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
                render_obstacles(&fb, &world, game.cam_x);
                t_after_obs = esp_timer_get_time();
                game_draw_ship(&fb, &game);
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
