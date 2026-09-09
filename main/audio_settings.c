#include "audio_settings.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "game_app.h"   // audio_reinstall_music
#include "se_audio.h"   // push toggles into the engine mixer's output gates

static char const TAG[]       = "audio_settings";
static char const NS[]        = "synthracer";
static char const KEY_MUSIC[] = "audio_music_on";
static char const KEY_SFX[]   = "audio_sfx_on";
static char const KEY_HUM[]   = "audio_hum_on";
static char const KEY_MP3[]   = "audio_music_mp3";

static bool s_music_on = true;
static bool s_sfx_on   = true;
static bool s_hum_on   = false;  // engine hum defaults off when unset in NVS
static bool s_music_mp3 = false; // procedural music is the default

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
    load_one(h, KEY_HUM,   &s_hum_on);
    load_one(h, KEY_MP3,   &s_music_mp3);
    nvs_close(h);
    ESP_LOGI(TAG, "Loaded audio settings: music=%d sfx=%d hum=%d mp3=%d",
             s_music_on, s_sfx_on, s_hum_on, s_music_mp3);
    return ESP_OK;
}

bool audio_settings_music_on(void) { return s_music_on; }
bool audio_settings_sfx_on(void)   { return s_sfx_on; }
bool audio_settings_hum_on(void)   { return s_hum_on; }
bool audio_settings_music_mp3(void) { return s_music_mp3; }

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
    audio_mixer_set_music_enabled(on);
}

void audio_settings_set_sfx_on(bool on) {
    if (s_sfx_on == on) return;
    s_sfx_on = on;
    save_one(KEY_SFX, on);
    audio_mixer_set_group_enabled(AUDIO_SFX_GROUP_GENERAL, on);
}

void audio_settings_set_hum_on(bool on) {
    if (s_hum_on == on) return;
    s_hum_on = on;
    save_one(KEY_HUM, on);
    audio_mixer_set_group_enabled(AUDIO_SFX_GROUP_HUM, on);
}

void audio_settings_set_music_mp3(bool on) {
    if (s_music_mp3 == on) return;
    s_music_mp3 = on;
    save_one(KEY_MP3, on);
    // Unlike the toggles above, this does not gate an output -- it picks
    // a different music_source_t. Swap the live one so the change is
    // audible immediately instead of at the next run.
    audio_reinstall_music();
}
