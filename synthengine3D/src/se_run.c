// =====================================================================
//  SynthEngine3D  --  application framework / run loop  (EF)
// ---------------------------------------------------------------------
//  Implements the inversion-of-control entry point declared in
//  include/se_run.h. The engine owns the device bootstrap (NVS, BSP,
//  display, double-buffered framebuffers, the 3D scene buffers, the
//  audio mixer, the vsync/tearing-effect semaphore) and the per-frame
//  loop (delta-time, callback dispatch, default backdrop clear, blit at
//  vsync, buffer swap). The game plugs in via se_app_callbacks_t.
//
//  STATUS (EF, staged migration): this is sub-step 1 -- the engine now
//  owns the loop, the framebuffers, blit and vsync. The input-queue pump
//  and the device-global keys (volume +/-, audio-jack, F1-exit) are NOT
//  yet consumed here; the game still drains input inside on_update. That
//  moves into se_run in the next sub-step, at which point on_input and
//  cfg.f1_exits become live. See ../devdocs/engine-extraction.md (EF).
// =====================================================================

#include "se_run.h"

#include "se_config.h"   // SE_FRAME_DT_MAX, SE_HW_VOLUME_STEP_PCT
#include "se_audio.h"    // audio_mixer_init, audio_mixer_shutdown
#include "se_hw.h"       // se_hw_init, se_hw_step_volume, se_hw_on_jack_event
#include "se_scene.h"    // scene_init

#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"   // bsp_input_get_queue, event/key enums
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "hal/lcd_types.h"
#include "nvs_flash.h"
#include "pax_gfx.h"

// ESP32-P4 PSRAM L2 cache line. The framebuffers live in PSRAM and are
// read by DMA (LCD scan-out) and by hardware blocks (e.g. a game's PPA
// backdrop), so they are allocated cache-line aligned and rounded up to
// a whole line so their tail does not share a line with neighbours.
#define SE_PSRAM_CACHE_LINE 128

static char const TAG[] = "se_run";

// ---- Engine-owned display + framebuffer state -----------------------
static se_display_info_t  s_di         = {0};
static pax_buf_t          s_fb_a       = {0};
static pax_buf_t          s_fb_b       = {0};
static pax_buf_t*         s_fb         = &s_fb_a;   // back buffer (drawn into)
static pax_buf_t*         s_fb_front   = &s_fb_b;   // front buffer (scanned out)
static void*              s_fb_a_px    = NULL;
static void*              s_fb_b_px    = NULL;
static SemaphoreHandle_t  s_vsync_sem  = NULL;

// Loop control. Set by se_request_exit(); the loop checks it once per
// frame and falls out (firing on_shutdown) when set.
static volatile bool      s_exit_requested = false;

// ---- Input pump state -----------------------------------------------
static QueueHandle_t      s_input_queue    = NULL;   // BSP event queue
static bool               s_f1_exits       = false;  // from se_app_config_t
// When true, every event is forwarded to on_input and the engine handles
// no device-global keys (used during a game's rebind capture).
static bool               s_input_passthrough = false;

void se_request_exit(void) {
    s_exit_requested = true;
}

void se_display_info(se_display_info_t* out) {
    if (out) *out = s_di;
}

void se_input_set_passthrough(bool on) {
    s_input_passthrough = on;
}

// Drain the BSP input queue once. The engine consumes the device-global
// keys live -- volume +/- (active output, via se_hw), the audio-jack
// re-route, and F1-exit when the game opted in -- and forwards every
// other event to on_input. The power button and F2/F3 are deliberately
// left untouched (the power button's 2 s-hold power-off lives in the
// coprocessor; function keys are too valuable to spend). While
// passthrough is on (rebind capture) the engine consumes nothing and
// forwards everything.
static void se_pump_input(se_app_callbacks_t const* cb, void* user) {
    if (s_input_queue == NULL) return;

    bsp_input_event_t ev;
    while (xQueueReceive(s_input_queue, &ev, 0) == pdTRUE) {
        // Audio-jack is pure device routing -- never a bindable key, so
        // the engine consumes it even during passthrough.
        if (ev.type == INPUT_EVENT_TYPE_ACTION &&
            ev.args_action.type == BSP_INPUT_ACTION_TYPE_AUDIO_JACK) {
            se_hw_on_jack_event(ev.args_action.state);
            continue;
        }
        // Volume +/- and F1-exit are consumed live, except under
        // passthrough (rebind capture), where they must reach the game
        // so they can be bound rather than acted on.
        if (!s_input_passthrough &&
            ev.type == INPUT_EVENT_TYPE_NAVIGATION && ev.args_navigation.state) {
            bsp_input_navigation_key_t const key = ev.args_navigation.key;
            if (key == BSP_INPUT_NAVIGATION_KEY_VOLUME_UP) {
                se_hw_step_volume(+SE_HW_VOLUME_STEP_PCT);
                continue;
            }
            if (key == BSP_INPUT_NAVIGATION_KEY_VOLUME_DOWN) {
                se_hw_step_volume(-SE_HW_VOLUME_STEP_PCT);
                continue;
            }
            if (key == BSP_INPUT_NAVIGATION_KEY_F1 && s_f1_exits) {
                // Return to launcher (audio down first). Does not return
                // under graceloader.
                audio_mixer_shutdown();
                bsp_device_restart_to_launcher();
                continue;
            }
        }
        // Not an engine-consumed event -- hand it to the game.
        if (cb->on_input) {
            cb->on_input(&ev, user);
        }
    }
}

// Present the back buffer: hand it to the LCD, wait for the vsync
// (tearing-effect) signal, then swap so the buffer just sent becomes the
// front (scanned out) and the previous front becomes the next back. The
// CPU and any hardware block only ever touch *s_fb, never the buffer the
// LCD is reading -- no tearing.
static void se_present(void) {
    bsp_display_blit(0, 0, s_di.width, s_di.height, pax_buf_get_pixels(s_fb));

    if (s_vsync_sem != NULL) {
        xSemaphoreTake(s_vsync_sem, pdMS_TO_TICKS(50));
    } else {
        vTaskDelay(pdMS_TO_TICKS(16));
    }

    pax_buf_t* tmp = s_fb;
    s_fb           = s_fb_front;
    s_fb_front     = tmp;
}

// Bootstrap NVS, BSP, the display, the framebuffers, the scene buffers,
// the audio mixer and vsync. Returns true on success; on failure logs
// and returns false (se_run then bails before the loop).
static bool se_bootstrap(void) {
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
        return false;
    }

    size_t                        h_res = 0, v_res = 0;
    lcd_color_rgb_pixel_format_t  color_format = LCD_COLOR_PIXEL_FORMAT_RGB888;
    lcd_rgb_data_endian_t         data_endian  = LCD_RGB_DATA_ENDIAN_LITTLE;
    res = bsp_display_get_parameters(&h_res, &v_res, &color_format, &data_endian);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get display parameters: %d", res);
        return false;
    }

    pax_buf_type_t format = PAX_BUF_24_888RGB;
    switch (color_format) {
        case LCD_COLOR_PIXEL_FORMAT_RGB565: format = PAX_BUF_16_565RGB; break;
        case LCD_COLOR_PIXEL_FORMAT_RGB888: format = PAX_BUF_24_888RGB; break;
        default: break;
    }

    pax_orientation_t orientation = PAX_O_UPRIGHT;
    switch (bsp_display_get_default_rotation()) {
        case BSP_DISPLAY_ROTATION_90:  orientation = PAX_O_ROT_CCW;  break;
        case BSP_DISPLAY_ROTATION_180: orientation = PAX_O_ROT_HALF; break;
        case BSP_DISPLAY_ROTATION_270: orientation = PAX_O_ROT_CW;   break;
        case BSP_DISPLAY_ROTATION_0:
        default:                       orientation = PAX_O_UPRIGHT;  break;
    }

    s_di.width       = h_res;
    s_di.height      = v_res;
    s_di.pax_format  = format;
    s_di.reversed    = (data_endian == LCD_RGB_DATA_ENDIAN_BIG);
    s_di.orientation = orientation;

    // Two PSRAM framebuffers, PPA/DMA-aligned, each wrapped in a pax_buf_t
    // so PAX rasterises straight into the raw layout the LCD reads.
    size_t const fb_size         = (size_t)h_res * v_res * 2u;
    size_t const aligned_fb_size =
        (fb_size + SE_PSRAM_CACHE_LINE - 1) & ~(size_t)(SE_PSRAM_CACHE_LINE - 1);
    s_fb_a_px = heap_caps_aligned_alloc(SE_PSRAM_CACHE_LINE, aligned_fb_size, MALLOC_CAP_SPIRAM);
    s_fb_b_px = heap_caps_aligned_alloc(SE_PSRAM_CACHE_LINE, aligned_fb_size, MALLOC_CAP_SPIRAM);
    if (s_fb_a_px == NULL || s_fb_b_px == NULL) {
        ESP_LOGE(TAG, "Failed to allocate framebuffers (a=%p b=%p)", s_fb_a_px, s_fb_b_px);
        return false;
    }

    pax_buf_init(&s_fb_a, s_fb_a_px, h_res, v_res, format);
    pax_buf_reversed(&s_fb_a, s_di.reversed);
    pax_buf_set_orientation(&s_fb_a, orientation);

    pax_buf_init(&s_fb_b, s_fb_b_px, h_res, v_res, format);
    pax_buf_reversed(&s_fb_b, s_di.reversed);
    pax_buf_set_orientation(&s_fb_b, orientation);

    s_fb       = &s_fb_a;
    s_fb_front = &s_fb_b;

    // 3D scene buffers (depth/stamp/line list) -- compile-time sized.
    scene_init();

    // Software audio mixer + I2S. Non-fatal: a failure just means silence.
    res = audio_mixer_init();
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "audio_mixer_init failed: %d -- audio will be silent", res);
    }

    // Device-global hardware settings (volume / brightness, jack routing).
    // After audio_mixer_init so its raw-jack amplifier default is overlaid
    // by the launcher-persisted volume + routing here.
    se_hw_init();

    // Input event queue (drained each frame by se_pump_input).
    res = bsp_input_get_queue(&s_input_queue);
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "bsp_input_get_queue failed: %d -- input will be dead", res);
        s_input_queue = NULL;
    }

    // Vsync via the panel tearing-effect line. Optional: without it the
    // loop falls back to a fixed delay (animation may stutter).
    esp_err_t te_err = bsp_display_set_tearing_effect_mode(BSP_DISPLAY_TE_V_BLANKING);
    if (te_err == ESP_OK) {
        te_err = bsp_display_get_tearing_effect_semaphore(&s_vsync_sem);
    }
    if (te_err != ESP_OK || s_vsync_sem == NULL) {
        ESP_LOGW(TAG, "Vsync not available -- animation may stutter");
        s_vsync_sem = NULL;
    }

    return true;
}

void se_run(se_app_config_t const* cfg, se_app_callbacks_t const* cb, void* user) {
    se_app_config_t lcfg = {0};
    if (cfg) lcfg = *cfg;

    s_exit_requested    = false;
    s_input_passthrough = false;
    s_f1_exits          = lcfg.f1_exits;

    if (cb == NULL || cb->on_update == NULL) {
        ESP_LOGE(TAG, "se_run: callbacks (and on_update) are required");
        return;
    }

    if (!se_bootstrap()) {
        return;
    }

    // Game-side init: world, save, content load, the game's own bootstrap
    // (backdrop caches, input, settings). Runs after the engine has the
    // display + audio + scene up, so se_display_info() is already valid.
    if (cb->on_init) {
        cb->on_init(user);
    }

    int64_t prev_us = esp_timer_get_time();
    while (!s_exit_requested) {
        int64_t const now_us = esp_timer_get_time();
        float         dt      = (float)(now_us - prev_us) / 1e6f;
        prev_us               = now_us;
        if (dt > SE_FRAME_DT_MAX) dt = SE_FRAME_DT_MAX;

        // Drain the input queue first: consume the device-global keys,
        // forward the rest to on_input so the game's per-frame consume
        // accessors see this frame's events in on_update below.
        se_pump_input(cb, user);

        // Per-frame game logic / state machine.
        cb->on_update(dt, user);

        // Backdrop: the game's hook, or a flat clear when none is set.
        if (cb->on_backdrop) {
            cb->on_backdrop(s_fb, user);
        } else {
            pax_background(s_fb, lcfg.backdrop_argb);
        }

        // Foreground: 3D scene, HUD, menus.
        if (cb->on_render) {
            cb->on_render(s_fb, user);
        }

        // Hand the finished frame to the LCD at vsync, then swap buffers.
        se_present();
    }

    if (cb->on_shutdown) {
        cb->on_shutdown(user);
    }
}
