#include "sfx/sfx_crash.h"

#include "se_audio_dsp.h"
#include "se_audio.h"
#include "se_audio_source.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdint.h>
#include <string.h>

static char const TAG[] = "sfx_crash";

// Two layers: filtered white-noise burst (the metallic crunch)
// and a sine sweeping from ~120 Hz down to ~40 Hz for the gut-
// punch low end. Both share a single envelope so they stay glued
// together.

#define CRASH_NOISE_LPF_HZ 1200.0f
#define CRASH_NOISE_HPF_HZ  150.0f
#define CRASH_SINE_F0_HZ   120.0f
#define CRASH_SINE_F1_HZ    40.0f
// Per-voice nominal amplitudes. Crash is the loudest one-shot
// by design (it's a kill cue) — set roughly equal here so the
// noise crunch and the low-end thud carry equal weight, then the
// SFX master gain (AUDIO_SFX_GAIN) scales the whole voice down
// at mix-down.
#define CRASH_AMP_NOISE     0.45f
#define CRASH_AMP_SINE      0.40f

#define CRASH_ATTACK_S   0.005f
#define CRASH_DECAY_S    0.50f

typedef struct {
    sfx_voice_t voice;
    audio_env_t env;
    uint32_t    noise_state;
    audio_biquad_t lpf;
    audio_biquad_t hpf;
    uint32_t    sine_phase;
    uint32_t    elapsed_samples;
    uint32_t    total_samples;
} crash_state_t;

static float lerpf(float a, float b, float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return a + (b - a) * t;
}

static void crash_render(sfx_voice_t* self, int16_t* out, size_t frames) {
    crash_state_t* st = (crash_state_t*)self;

    for (size_t i = 0; i < frames; i++) {
        float const env = audio_env_tick(&st->env);

        // Sine pitch slides from F0 down to F1 over the decay.
        float const t  = (float)st->elapsed_samples / (float)st->total_samples;
        float const f  = lerpf(CRASH_SINE_F0_HZ, CRASH_SINE_F1_HZ, t);
        uint32_t const sine_inc = audio_dsp_phase_inc(f);

        // Noise layer.
        float n = audio_dsp_noise(&st->noise_state);
        n = audio_biquad_tick(&st->hpf, n);
        n = audio_biquad_tick(&st->lpf, n);
        n *= CRASH_AMP_NOISE;

        // Sine layer.
        float s = audio_dsp_sin(st->sine_phase) * CRASH_AMP_SINE;
        st->sine_phase += sine_inc;

        float const mix = (n + s) * env;
        int16_t const sample = audio_dsp_to_s16(mix);
        out[2 * i + 0] = sample;
        out[2 * i + 1] = sample;

        st->elapsed_samples++;
    }

    if (audio_env_is_idle(&st->env)) {
        self->finished = true;
    }
}

static void crash_shutdown(sfx_voice_t* self) {
    heap_caps_free(self);
}

bool sfx_crash_play(void) {
    crash_state_t* st = (crash_state_t*)heap_caps_calloc(1, sizeof(*st), MALLOC_CAP_INTERNAL);
    if (st == NULL) {
        ESP_LOGW(TAG, "alloc failed");
        return false;
    }

    st->voice.render   = crash_render;
    st->voice.shutdown = crash_shutdown;
    st->voice.finished = false;
    st->noise_state    = 0xC0FFEEu;
    st->total_samples  = (uint32_t)((CRASH_ATTACK_S + CRASH_DECAY_S) * (float)AUDIO_SAMPLE_RATE_HZ);
    audio_biquad_lpf(&st->lpf, CRASH_NOISE_LPF_HZ, 0.7f);
    audio_biquad_hpf(&st->hpf, CRASH_NOISE_HPF_HZ, 0.7f);
    audio_env_configure(&st->env, CRASH_ATTACK_S, CRASH_DECAY_S, 0.0f, 0.001f);
    audio_env_trigger(&st->env);

    if (!audio_mixer_register_voice(&st->voice)) {
        heap_caps_free(st);
        return false;
    }
    return true;
}
