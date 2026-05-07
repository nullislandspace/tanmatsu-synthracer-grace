// Race the Synth — Tanmatsu graceloader app.
// Phase 1: synthwave backdrop only. Press F1 to exit to launcher.

#include <stdio.h>

#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/lcd_types.h"
#include "nvs_flash.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"
#include "icons.h"
#include "rendertext.h"
#include "synthwave.h"

static char const TAG[] = "racethesynth";

static size_t                       display_h_res        = 0;
static size_t                       display_v_res        = 0;
static lcd_color_rgb_pixel_format_t display_color_format = LCD_COLOR_PIXEL_FORMAT_RGB888;
static lcd_rgb_data_endian_t        display_data_endian  = LCD_RGB_DATA_ENDIAN_LITTLE;
static pax_buf_t                    fb                   = {0};

static void blit(void) {
    bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&fb));
}

void app_main(void) {
    // NVS — reused later for highscore/level/seed/volume; init early.
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

    // Pre-triangulate the synthwave polygons once.
    synthwave_init();

    // Load function-key icons from /int/icons/. Tolerated to fail.
    icons_load();

    // Vsync — pace the loop to the panel's V-blanking edge so the grid
    // scroll doesn't tear. Falls back to a 50 ms wait if unavailable.
    SemaphoreHandle_t vsync_sem = NULL;
    esp_err_t         te_err    = bsp_display_set_tearing_effect_mode(BSP_DISPLAY_TE_V_BLANKING);
    if (te_err == ESP_OK) {
        te_err = bsp_display_get_tearing_effect_semaphore(&vsync_sem);
    }
    if (te_err != ESP_OK || vsync_sem == NULL) {
        ESP_LOGW(TAG, "Vsync not available — animation may stutter");
        vsync_sem = NULL;
    }

    QueueHandle_t input_event_queue = NULL;
    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));

    ESP_LOGI(TAG, "Race the Synth: Phase 1 skeleton ready");

    while (1) {
        // Drain input events. For Phase 1 the only binding is F1 → exit.
        bsp_input_event_t event;
        while (xQueueReceive(input_event_queue, &event, 0) == pdTRUE) {
            if (event.type == INPUT_EVENT_TYPE_NAVIGATION && event.args_navigation.state &&
                event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_F1) {
                bsp_device_restart_to_launcher();
            }
        }

        // Per-frame draw order (matters for sun-behind-mountains occlusion):
        synthwave_draw_sky(&fb);
        synthwave_draw_sun(&fb, 0.0f);
        synthwave_draw_mountains(&fb);
        synthwave_draw_wireframe(&fb);
        synthwave_draw_top_grid(&fb);
        synthwave_step(&fb, 1);

        // Phase 1 prompt — replaced by the title screen in Phase 2+.
        // Hershey font draws via direct pixel writes, much cheaper than
        // pax_draw_text on the per-frame path.
        char const* title = "Race the Synth";
        pax_vec2f   tsz   = rendertext_size(NULL, 32, title);
        rendertext_draw(&fb, 0xFFFFFFFF, NULL, 32, 400 - tsz.x / 2, 380, title);

        // "[F1] to exit" — use the loaded function-key icon when available
        // (the Tanmatsu keyboard prints symbols, not "F1" text), with a
        // text fallback if the icon is missing.
        char const* prompt   = "to exit";
        float const prompt_h = 18.0f;
        pax_vec2f   psz      = rendertext_size(NULL, prompt_h, prompt);
        int         icon_w   = icons_width(ICON_F1);
        if (icon_w > 0) {
            float const gap        = 8.0f;
            float       total_w    = (float)icon_w + gap + psz.x;
            float       icon_x     = 400.0f - total_w / 2.0f;
            int         icon_h     = icons_height(ICON_F1);
            // Vertical-center the icon against the text baseline.
            float       icon_y     = 425.0f + prompt_h / 2.0f - (float)icon_h / 2.0f;
            float       text_x     = icon_x + (float)icon_w + gap;
            icons_blit(&fb, ICON_F1, icon_x, icon_y);
            rendertext_draw(&fb, 0xFFFFFFFF, NULL, prompt_h, text_x, 425, prompt);
        } else {
            char const* fallback   = "Press F1 to exit";
            pax_vec2f   fsz        = rendertext_size(NULL, prompt_h, fallback);
            rendertext_draw(&fb, 0xFFFFFFFF, NULL, prompt_h, 400 - fsz.x / 2, 425, fallback);
        }

        blit();

        if (vsync_sem != NULL) {
            xSemaphoreTake(vsync_sem, pdMS_TO_TICKS(50));
        } else {
            vTaskDelay(pdMS_TO_TICKS(16));
        }
    }
}
