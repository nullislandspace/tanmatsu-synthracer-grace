#include "audio_mixer.h"

#include "audio_dsp.h"
#include "audio_settings.h"

#include "bsp/audio.h"
#include "bsp/input.h"
#include "driver/i2s_common.h"
#include "driver/i2s_types.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdint.h>
#include <string.h>

static char const TAG[] = "audio_mixer";

// Pipeline format.
#define MIXER_FRAME_BYTES   4                              // s16 stereo
#define MIXER_CHUNK_FRAMES  256                            // ~11.6 ms @ 22050
#define MIXER_CHUNK_SAMPLES (MIXER_CHUNK_FRAMES * 2)       // L+R
#define MIXER_CHUNK_BYTES   (MIXER_CHUNK_FRAMES * MIXER_FRAME_BYTES)

// Music is mixed at ~30% so SFX have headroom. Q15 scale factor.
#define MIXER_MUSIC_GAIN_Q15 ((int32_t)(0.30f * 32768.0f))

// Number of consecutive silent chunks the mixer pushes through the
// I2S DMA queue before powering down. ~46 ms covers the worst case
// of DMA queue + codec latency without chopping the tail off a
// just-finished SFX.
#define MIXER_DRAIN_CHUNKS 4   // 4 * 256 frames @ 22050 ≈ 46 ms

// Voice slots — N concurrent SFX. Engine hum + scrape can be
// persistent; one-shots flow through whatever's free. 8 is
// generous: typical worst case is hum + scrape + one or two
// one-shots overlapping.
#define MIXER_VOICE_SLOTS 8

#define MIXER_TASK_STACK_WORDS 6144
#define MIXER_TASK_PRIORITY    (configMAX_PRIORITIES - 2)  // just under timer/IDLE-pinning priorities

typedef struct {
    sfx_voice_t* voice;
} voice_slot_t;

static music_source_t* g_music = NULL;
static voice_slot_t    g_voices[MIXER_VOICE_SLOTS];

static SemaphoreHandle_t g_slots_mutex = NULL;
static i2s_chan_handle_t g_i2s         = NULL;
static TaskHandle_t      g_mixer_task  = NULL;
static bool              g_initialised = false;
static volatile bool     g_shutdown    = false;
static bool              g_powered_on  = true;

// Scratch buffers — static so they don't burn task-stack space.
// `g_mix_*` is the per-source render target; `g_accum` is the
// signed accumulator before saturation; `g_out` is the int16
// chunk we hand to I2S.
static int16_t g_mix_music[MIXER_CHUNK_SAMPLES];
static int16_t g_mix_voice[MIXER_CHUNK_SAMPLES];
static int32_t g_accum[MIXER_CHUNK_SAMPLES];
static int16_t g_out[MIXER_CHUNK_SAMPLES];

// ---------------------------------------------------------------
// Power management — only called from the mixer task.
// ---------------------------------------------------------------

static void power_up(void) {
    if (g_powered_on) return;

    esp_err_t err = i2s_channel_enable(g_i2s);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(err));
    }

    bool jack_inserted = false;
    if (bsp_input_read_action(BSP_INPUT_ACTION_TYPE_AUDIO_JACK, &jack_inserted) != ESP_OK) {
        jack_inserted = false;
    }
    bsp_audio_set_amplifier(!jack_inserted);

    g_powered_on = true;
    ESP_LOGD(TAG, "Audio resumed");
}

static void power_down(void) {
    if (!g_powered_on) return;

    bsp_audio_set_amplifier(false);

    esp_err_t err = i2s_channel_disable(g_i2s);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2s_channel_disable failed: %s", esp_err_to_name(err));
    }

    g_powered_on = false;
    ESP_LOGD(TAG, "Audio idle — powered down");
}

// ---------------------------------------------------------------
// Voice slot helpers — caller holds g_slots_mutex.
// ---------------------------------------------------------------

static int find_free_slot_locked(void) {
    for (int i = 0; i < MIXER_VOICE_SLOTS; i++) {
        if (g_voices[i].voice == NULL) return i;
    }
    return -1;
}

static void reap_finished_voices_locked(void) {
    for (int i = 0; i < MIXER_VOICE_SLOTS; i++) {
        sfx_voice_t* v = g_voices[i].voice;
        if (v != NULL && v->finished) {
            if (v->shutdown) v->shutdown(v);
            g_voices[i].voice = NULL;
        }
    }
}

// ---------------------------------------------------------------
// Mixer task
// ---------------------------------------------------------------

static void mixer_task_fn(void* arg) {
    (void)arg;
    int silence_chunks = 0;

    while (1) {
        if (g_shutdown) {
            // Final shutdown path: drop into a permanent block.
            // amplifier + I2S are already muted/disabled in
            // audio_mixer_shutdown().
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        bool const music_gate = audio_settings_music_on();
        bool const sfx_gate   = audio_settings_sfx_on();
        int active_sources = 0;

        memset(g_accum, 0, sizeof(g_accum));

        // ---- Music render + scale into accum at 30% ----
        xSemaphoreTake(g_slots_mutex, portMAX_DELAY);
        music_source_t* music = g_music;
        if (music_gate && music != NULL && music->render != NULL) {
            memset(g_mix_music, 0, sizeof(g_mix_music));
            music->render(music, g_mix_music, MIXER_CHUNK_FRAMES);
            for (size_t j = 0; j < MIXER_CHUNK_SAMPLES; j++) {
                g_accum[j] += ((int32_t)g_mix_music[j] * MIXER_MUSIC_GAIN_Q15) >> 15;
            }
            active_sources++;
        }

        // ---- SFX voices ----
        if (sfx_gate) {
            for (int i = 0; i < MIXER_VOICE_SLOTS; i++) {
                sfx_voice_t* v = g_voices[i].voice;
                if (v == NULL || v->finished || v->render == NULL) continue;
                memset(g_mix_voice, 0, sizeof(g_mix_voice));
                v->render(v, g_mix_voice, MIXER_CHUNK_FRAMES);
                for (size_t j = 0; j < MIXER_CHUNK_SAMPLES; j++) {
                    g_accum[j] += g_mix_voice[j];
                }
                active_sources++;
            }
        }

        // ---- Reap finished SFX voices ----
        reap_finished_voices_locked();
        xSemaphoreGive(g_slots_mutex);

        // ---- Idle handling ----
        if (active_sources == 0) {
            if (!g_powered_on) {
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                continue;
            }
            if (silence_chunks < MIXER_DRAIN_CHUNKS) {
                memset(g_out, 0, sizeof(g_out));
                size_t written = 0;
                i2s_channel_write(g_i2s, g_out, sizeof(g_out), &written, portMAX_DELAY);
                silence_chunks++;
                continue;
            }
            power_down();
            silence_chunks = 0;
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        // Power back up before the first non-silent chunk goes out.
        if (!g_powered_on) power_up();
        silence_chunks = 0;

        // ---- Saturate accum → out ----
        for (size_t j = 0; j < MIXER_CHUNK_SAMPLES; j++) {
            int32_t s = g_accum[j];
            if (s >  32767) s =  32767;
            if (s < -32768) s = -32768;
            g_out[j] = (int16_t)s;
        }

        size_t written = 0;
        i2s_channel_write(g_i2s, g_out, sizeof(g_out), &written, portMAX_DELAY);
    }
}

// ---------------------------------------------------------------
// Public API
// ---------------------------------------------------------------

esp_err_t audio_mixer_init(void) {
    if (g_initialised) return ESP_OK;

    esp_err_t err = audio_dsp_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio_dsp_init failed: %d", err);
        return err;
    }

    err = bsp_audio_set_rate(AUDIO_SAMPLE_RATE_HZ);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_set_rate failed: %d", err);
        return err;
    }

    err = bsp_audio_get_i2s_handle(&g_i2s);
    if (err != ESP_OK || g_i2s == NULL) {
        ESP_LOGE(TAG, "Failed to get I2S handle: %d", err);
        return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
    }

    // Match the launcher's amp policy: speaker on unless headphones
    // are inserted.
    bool jack = false;
    if (bsp_input_read_action(BSP_INPUT_ACTION_TYPE_AUDIO_JACK, &jack) != ESP_OK) jack = false;
    bsp_audio_set_amplifier(!jack);

    g_slots_mutex = xSemaphoreCreateMutex();
    if (g_slots_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mixer mutex");
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < MIXER_VOICE_SLOTS; i++) g_voices[i].voice = NULL;
    g_music       = NULL;
    g_powered_on  = true;
    g_shutdown    = false;

    BaseType_t ok = xTaskCreatePinnedToCore(
        mixer_task_fn, "audio_mixer",
        MIXER_TASK_STACK_WORDS, NULL,
        MIXER_TASK_PRIORITY, &g_mixer_task,
        1  // core 1 — leaves core 0 for the render loop
    );
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create mixer task");
        vSemaphoreDelete(g_slots_mutex);
        g_slots_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    g_initialised = true;
    ESP_LOGI(TAG, "Audio mixer started (%d Hz, %d frames/chunk, %d voices)",
             AUDIO_SAMPLE_RATE_HZ, MIXER_CHUNK_FRAMES, MIXER_VOICE_SLOTS);
    return ESP_OK;
}

void audio_mixer_shutdown(void) {
    if (!g_initialised) return;

    // Tell the mixer task to stop touching the hardware *before* we
    // disable I2S, so it doesn't immediately try to power it back
    // up. Clear the music slot and all voices so render passes
    // produce no work.
    g_shutdown = true;

    xSemaphoreTake(g_slots_mutex, portMAX_DELAY);
    if (g_music != NULL) {
        if (g_music->shutdown) g_music->shutdown(g_music);
        g_music = NULL;
    }
    for (int i = 0; i < MIXER_VOICE_SLOTS; i++) {
        sfx_voice_t* v = g_voices[i].voice;
        if (v != NULL) {
            if (v->shutdown) v->shutdown(v);
            g_voices[i].voice = NULL;
        }
    }
    xSemaphoreGive(g_slots_mutex);

    // Wake the mixer task so it sees the shutdown flag and parks.
    if (g_mixer_task != NULL) xTaskNotifyGive(g_mixer_task);

    // Synchronously silence the hardware right now. Subsequent
    // calls into the mixer are no-ops by design.
    bsp_audio_set_amplifier(false);
    if (g_i2s != NULL) {
        esp_err_t err = i2s_channel_disable(g_i2s);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2s_channel_disable in shutdown failed: %s", esp_err_to_name(err));
        }
    }
    g_powered_on = false;
    ESP_LOGI(TAG, "Audio shutdown — amp + I2S off");
}

void audio_mixer_set_music(music_source_t* src) {
    if (!g_initialised || g_shutdown) return;

    music_source_t* prev = NULL;
    xSemaphoreTake(g_slots_mutex, portMAX_DELAY);
    prev    = g_music;
    g_music = src;
    xSemaphoreGive(g_slots_mutex);

    if (prev != NULL && prev->shutdown != NULL) {
        prev->shutdown(prev);
    }

    if (src != NULL && g_mixer_task != NULL) {
        xTaskNotifyGive(g_mixer_task);
    }
}

bool audio_mixer_register_voice(sfx_voice_t* v) {
    if (!g_initialised || g_shutdown || v == NULL) return false;

    bool placed = false;
    xSemaphoreTake(g_slots_mutex, portMAX_DELAY);
    int idx = find_free_slot_locked();
    if (idx >= 0) {
        v->finished     = false;
        g_voices[idx].voice = v;
        placed          = true;
    }
    xSemaphoreGive(g_slots_mutex);

    if (placed && g_mixer_task != NULL) {
        xTaskNotifyGive(g_mixer_task);
    } else if (!placed) {
        ESP_LOGW(TAG, "All SFX voice slots full — dropping voice");
    }
    return placed;
}

void audio_mixer_stop_voice(sfx_voice_t* v) {
    if (!g_initialised || v == NULL) return;
    // We don't unhook the slot here — the mixer task does that on
    // its next pass via reap_finished_voices_locked(). Marking the
    // flag is just a hint that lives next to the voice struct.
    v->finished = true;
}

void audio_mixer_stop_all_voices(void) {
    if (!g_initialised) return;
    xSemaphoreTake(g_slots_mutex, portMAX_DELAY);
    for (int i = 0; i < MIXER_VOICE_SLOTS; i++) {
        if (g_voices[i].voice != NULL) {
            g_voices[i].voice->finished = true;
        }
    }
    xSemaphoreGive(g_slots_mutex);
}
