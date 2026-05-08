// Race the Synth — Tanmatsu graceloader app.
//
// Phase 2: synthwave backdrop + steerable ship at the bottom of the screen.
// F1 exits to the launcher. No obstacles, scoring, or sun timer yet — those
// come in later phases. See DEVELOPMENT.md for the full plan.

#include <stdio.h>
#include <string.h>

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

static void blit(void) {
    bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&fb));
}

static void draw_speed_readout(float speed_z) {
    // Top-right debug overlay. Tuning aid for matching the floor's
    // visible motion to a comfortable forward speed; cursor up/down
    // adjust ship_speed_z live. Use pax_buf_get_widthf so the offset
    // respects the post-orientation logical width rather than the
    // raw display_h_res, which is the un-rotated physical width.
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
    input_set_mode(INPUT_MODE_PLAYING);

    // Allocate the backdrop cache in PSRAM (the main fb is also there).
    // We keep our own pointer so we can memcpy into the main fb without
    // going through PAX's per-pixel draw path.
    backdrop_size   = pax_buf_get_size(&fb);
    backdrop_pixels = heap_caps_malloc(backdrop_size, MALLOC_CAP_SPIRAM);
    if (backdrop_pixels == NULL) {
        ESP_LOGE(TAG, "Failed to allocate %u bytes for backdrop cache", (unsigned)backdrop_size);
        return;
    }
    pax_buf_init(&backdrop_cache, backdrop_pixels, display_h_res, display_v_res, format);
    pax_buf_reversed(&backdrop_cache, display_data_endian == LCD_RGB_DATA_ENDIAN_BIG);
    pax_buf_set_orientation(&backdrop_cache, orientation);

    // Render static layers into the cache once. The sun's `dy` will
    // start changing in Phase 5 (sunset mechanic); when it does, this
    // call will need to be re-issued whenever dy moves enough to
    // matter visually.
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

    game_state_t game;
    game_init(&game);

    world_state_t world;
    // Seed from boot time for now. Phase 8 will replace this with the
    // RTC-derived daily seed (or the player's custom seed).
    world_init(&world, (uint32_t)(esp_timer_get_time() & 0xFFFFFFFFu) | 1u);

    int64_t prev_us = esp_timer_get_time();

    ESP_LOGI(TAG, "Race the Synth: Phase 3 obstacles ready");

    while (1) {
        if (input_drain_events()) {
            bsp_device_restart_to_launcher();
        }

        int64_t now_us = esp_timer_get_time();
        float   dt     = (float)(now_us - prev_us) / 1e6f;
        prev_us        = now_us;
        // Cap dt to keep physics sane after a long stall (e.g. backlight
        // dim or a paged-out frame).
        if (dt > 0.1f) dt = 0.1f;

        int sd = input_consume_speed_delta();
        if (sd != 0) {
            game.ship_speed_z += (float)sd * 1.0f;
            if (game.ship_speed_z < 0.5f) game.ship_speed_z = 0.5f;
            if (game.ship_speed_z > 60.0f) game.ship_speed_z = 60.0f;
        }

        int steer = input_steering();
        game_step(&game, dt, steer);
        world_advance(&world, dt, game.ship_speed_z);

        // Static synthwave layers come from the pre-rendered cache.
        // We bypass PAX's draw path with a single memcpy — the cache and
        // the main fb share the same dimensions, format, orientation
        // and endianness, so the byte layout is identical.
        memcpy((void*)pax_buf_get_pixels(&fb), backdrop_pixels, backdrop_size);

        // Dynamic content drawn on top of the cached backdrop.
        // Floor scroll is coupled to forward speed so obstacles appear
        // anchored to the floor texture as they approach. cam_x pans
        // the floor's lane lines in world space.
        synthwave_step(&fb, game.ship_speed_z * dt, game.cam_x);

        // Obstacles render between the floor and the ship: floor goes
        // behind everything, ship is always in front.
        render_obstacles(&fb, &world, game.cam_x);

        game_draw_ship(&fb, &game);

        draw_exit_hint();
        draw_speed_readout(game.ship_speed_z);

        blit();

        if (vsync_sem != NULL) {
            xSemaphoreTake(vsync_sem, pdMS_TO_TICKS(50));
        } else {
            vTaskDelay(pdMS_TO_TICKS(16));
        }
    }
}
