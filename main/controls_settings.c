#include "controls_settings.h"

#include "bsp/input.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static char const TAG[] = "controls_settings";
static char const NS[]  = "synthracer";

static char const KEY_GYRO[]  = "ctl_gyro";
// One NVS key per remappable action, indexed by controls_key_t.
static char const* const KEY_BIND[CONTROL_KEY_COUNT] = {
    [CONTROL_KEY_LEFT]  = "ctl_k_left",
    [CONTROL_KEY_RIGHT] = "ctl_k_right",
    [CONTROL_KEY_ITEM]  = "ctl_k_item",
    [CONTROL_KEY_PAUSE] = "ctl_k_pause",
};

// Defaults — see header. Match the bindings that were hard-coded
// before the Controls menu existed.
static uint16_t const DEFAULT_BIND[CONTROL_KEY_COUNT] = {
    [CONTROL_KEY_LEFT]  = BSP_INPUT_SCANCODE_ESC,        // 0x01
    [CONTROL_KEY_RIGHT] = BSP_INPUT_SCANCODE_BACKSPACE,  // 0x0E
    [CONTROL_KEY_ITEM]  = BSP_INPUT_SCANCODE_SPACE,      // 0x39
    [CONTROL_KEY_PAUSE] = BSP_INPUT_SCANCODE_F4,         // 0x3E
};

static bool     s_gyro_on = false;
static uint16_t s_bind[CONTROL_KEY_COUNT];

esp_err_t controls_settings_load(void) {
    // Start from defaults so a missing key (or missing namespace)
    // leaves a sane value.
    for (int i = 0; i < CONTROL_KEY_COUNT; i++) {
        s_bind[i] = DEFAULT_BIND[i];
    }
    s_gyro_on = false;

    nvs_handle_t h;
    esp_err_t    err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;  // namespace doesn't exist yet — defaults stand
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open(%s) failed: %d (using defaults)", NS, err);
        return err;
    }

    uint8_t g = 0;
    if (nvs_get_u8(h, KEY_GYRO, &g) == ESP_OK) {
        s_gyro_on = (g != 0);
    }
    for (int i = 0; i < CONTROL_KEY_COUNT; i++) {
        uint16_t v = 0;
        if (nvs_get_u16(h, KEY_BIND[i], &v) == ESP_OK && v != 0) {
            s_bind[i] = v;
        }
    }
    nvs_close(h);
    ESP_LOGI(TAG, "Loaded controls: gyro=%d left=0x%02X right=0x%02X item=0x%02X pause=0x%02X",
             s_gyro_on, s_bind[CONTROL_KEY_LEFT], s_bind[CONTROL_KEY_RIGHT],
             s_bind[CONTROL_KEY_ITEM], s_bind[CONTROL_KEY_PAUSE]);
    return ESP_OK;
}

bool controls_settings_gyro_on(void) { return s_gyro_on; }

uint16_t controls_settings_key(controls_key_t which) {
    if (which < 0 || which >= CONTROL_KEY_COUNT) return 0;
    return s_bind[which];
}

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

static void save_u16(char const* key, uint16_t value) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open(%s, RW) failed", NS);
        return;
    }
    if (nvs_set_u16(h, key, value) == ESP_OK) {
        nvs_commit(h);
    } else {
        ESP_LOGW(TAG, "nvs_set_u16(%s) failed", key);
    }
    nvs_close(h);
}

void controls_settings_set_gyro_on(bool on) {
    if (s_gyro_on == on) return;
    s_gyro_on = on;
    save_u8(KEY_GYRO, on ? 1 : 0);
}

void controls_settings_set_key(controls_key_t which, uint16_t scancode) {
    if (which < 0 || which >= CONTROL_KEY_COUNT) return;
    if (scancode == 0 || s_bind[which] == scancode) return;
    s_bind[which] = scancode;
    save_u16(KEY_BIND[which], scancode);
}
