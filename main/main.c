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
                .requested_color_format = LCD_COLOR_PIXEL_FORMAT_RGB888,
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
    backdrop_pixels = heap_caps_malloc(backdrop_size, MALLOC_CAP_SPIRAM);
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

    ESP_LOGI(TAG, "Race the Synth: title screen up");

    while (1) {
        if (input_drain_events()) {
            bsp_device_restart_to_launcher();
        }

        int64_t now_us = esp_timer_get_time();
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

        // Frame setup: start with the cached backdrop and draw the
        // scrolling floor. The floor scroll speed depends on the
        // app state — fixed in TITLE, ship-driven in PLAYING,
        // frozen in GAME_OVER.
        memcpy((void*)pax_buf_get_pixels(&fb), backdrop_pixels, backdrop_size);

        switch (app_state) {
            case APP_STATE_TITLE: {
                synthwave_step(&fb, title_scroll_speed * dt, 0.0f);
                draw_title_overlay();
                draw_exit_hint();
                if (pickup_pressed) {
                    start_run(&game, &world, run_seed);
                    app_state = APP_STATE_PLAYING;
                }
                break;
            }

            case APP_STATE_PLAYING: {
                // 1. Apply bank + lateral motion using this frame's
                //    steer input.
                // 2. Collide against the world: push the ship out
                //    of any side-contact obstacle and set scrape
                //    flags (or return head-on).
                // 3. After-collide work that reads the flags:
                //    ramp ship_speed_z, emit + advance sparks.
                // Doing collide *after* motion makes the resolved
                // position the one we render and the one that
                // feeds the next world_advance.
                game_step(&game, dt, steer);
                bool const head_on = game_collide(&game, &world, dt);
                game_after_collide(&game, dt);
                world_advance(&world, dt, game.ship_speed_z);

                synthwave_step(&fb, game.ship_speed_z * dt, game.cam_x);
                render_obstacles(&fb, &world, game.cam_x);
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
                synthwave_step(&fb, 0.0f, game.cam_x);
                render_obstacles(&fb, &world, game.cam_x);
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

        blit();

        if (vsync_sem != NULL) {
            xSemaphoreTake(vsync_sem, pdMS_TO_TICKS(50));
        } else {
            vTaskDelay(pdMS_TO_TICKS(16));
        }
    }
}
