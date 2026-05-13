#include "audio_settings.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static char const TAG[]       = "audio_settings";
static char const NS[]        = "synthracer";
static char const KEY_MUSIC[] = "audio_music_on";
static char const KEY_SFX[]   = "audio_sfx_on";

static bool s_music_on = true;
static bool s_sfx_on   = true;

static void load_one(nvs_handle_t h, char const* key, bool* out) {
    uint8_t v = 1;
    esp_err_t err = nvs_get_u8(h, key, &v);
    if (err == ESP_OK) {
        *out = (v != 0);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "nvs_get_u8(%s) failed: %d (using default on)", key, err);
    }
}

esp_err_t audio_settings_load(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // Namespace doesn't exist yet — use defaults.
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open(%s) failed: %d (using defaults)", NS, err);
        return err;
    }
    load_one(h, KEY_MUSIC, &s_music_on);
    load_one(h, KEY_SFX,   &s_sfx_on);
    nvs_close(h);
    ESP_LOGI(TAG, "Loaded audio settings: music=%d sfx=%d", s_music_on, s_sfx_on);
    return ESP_OK;
}

bool audio_settings_music_on(void) { return s_music_on; }
bool audio_settings_sfx_on(void)   { return s_sfx_on; }

static void save_one(char const* key, bool value) {
    nvs_handle_t h;
    esp_err_t    err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open(%s, RW) failed: %d", NS, err);
        return;
    }
    err = nvs_set_u8(h, key, value ? 1 : 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_set_u8(%s) failed: %d", key, err);
    } else {
        err = nvs_commit(h);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "nvs_commit failed: %d", err);
        }
    }
    nvs_close(h);
}

void audio_settings_set_music_on(bool on) {
    if (s_music_on == on) return;
    s_music_on = on;
    save_one(KEY_MUSIC, on);
}

void audio_settings_set_sfx_on(bool on) {
    if (s_sfx_on == on) return;
    s_sfx_on = on;
    save_one(KEY_SFX, on);
}
