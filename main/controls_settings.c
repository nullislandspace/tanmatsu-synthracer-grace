#include "controls_settings.h"

#include "bsp/input.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "se_bindings.h"

static char const TAG[] = "controls_settings";
static char const NS[]  = "synthracer";

static char const KEY_GYRO[] = "ctl_gyro";

// The remappable controls, declared for the engine's se_bindings
// subsystem. Same NVS namespace + keys + defaults the game used before
// the bindings moved into the engine, so a player's remapped keys carry
// over. Order matches controls_key_t (the Controls menu's row order).
static se_binding_def_t const BINDINGS[CONTROL_KEY_COUNT] = {
    [CONTROL_KEY_LEFT]  = { CONTROL_KEY_LEFT,  "Left",     "ctl_k_left",  BSP_INPUT_SCANCODE_ESC       },
    [CONTROL_KEY_RIGHT] = { CONTROL_KEY_RIGHT, "Right",    "ctl_k_right", BSP_INPUT_SCANCODE_BACKSPACE },
    [CONTROL_KEY_ITEM]  = { CONTROL_KEY_ITEM,  "Use item", "ctl_k_item",  BSP_INPUT_SCANCODE_SPACE     },
    [CONTROL_KEY_PAUSE] = { CONTROL_KEY_PAUSE, "Pause",    "ctl_k_pause", BSP_INPUT_SCANCODE_F4        },
};
static se_bindings_config_t const BINDINGS_CFG = {
    .nvs_namespace = NS,
    .defs          = BINDINGS,
    .count         = CONTROL_KEY_COUNT,
};

static bool s_gyro_on = false;

esp_err_t controls_settings_load(void) {
    // Hand the keybinds to the engine (loads persisted scancodes, else
    // the declared defaults).
    se_bindings_init(&BINDINGS_CFG);

    // Gyro flag stays a game setting (a toggle, not a binding).
    s_gyro_on = false;
    nvs_handle_t h;
    esp_err_t    err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;  // namespace doesn't exist yet — default stands
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open(%s) failed: %d (gyro defaults off)", NS, err);
        return err;
    }
    uint8_t g = 0;
    if (nvs_get_u8(h, KEY_GYRO, &g) == ESP_OK) {
        s_gyro_on = (g != 0);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "Loaded controls: gyro=%d (keybinds via se_bindings)", s_gyro_on);
    return ESP_OK;
}

bool controls_settings_gyro_on(void) { return s_gyro_on; }

static void save_u8(char const* key, uint8_t value) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open(%s, RW) failed", NS);
        return;
    }
    if (nvs_set_u8(h, key, value) == ESP_OK) {
        nvs_commit(h);
    } else {
        ESP_LOGW(TAG, "nvs_set_u8(%s) failed", key);
    }
    nvs_close(h);
}

void controls_settings_set_gyro_on(bool on) {
    if (s_gyro_on == on) return;
    s_gyro_on = on;
    save_u8(KEY_GYRO, on ? 1 : 0);
}
