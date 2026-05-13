#include "sfx/sfx_cube_bump.h"

#include "audio_dsp.h"
#include "audio_mixer.h"
#include "audio_source.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdint.h>
#include <string.h>

static char const TAG[] = "sfx_bump";

// Pitched sine drop from ~110 Hz to ~50 Hz with a fast attack
// and a 0.25 s decay gives a satisfying "thump". A tiny lowpassed
// noise transient layered over the top sells the impact.

#define BUMP_SINE_F0_HZ 110.0f
#define BUMP_SINE_F1_HZ  50.0f
#define BUMP_NOISE_LPF_HZ 800.0f
#define BUMP_ATTACK_S 0.003f
#define BUMP_DECAY_S  0.25f
#define BUMP_AMP_SINE   0.75f
#define BUMP_AMP_NOISE  0.20f

typedef struct {
    sfx_voice_t voice;
    audio_env_t env;
    uint32_t    noise_state;
    audio_biquad_t lpf;
    uint32_t    sine_phase;
    uint32_t    elapsed_samples;
    uint32_t    total_samples;
} bump_state_t;

static float lerpf(float a, float b, float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return a + (b - a) * t;
}

static void bump_render(sfx_voice_t* self, int16_t* out, size_t frames) {
    bump_state_t* st = (bump_state_t*)self;

    for (size_t i = 0; i < frames; i++) {
        float const env = audio_env_tick(&st->env);

        float const t = (float)st->elapsed_samples / (float)st->total_samples;
        float const f = lerpf(BUMP_SINE_F0_HZ, BUMP_SINE_F1_HZ, t);
        uint32_t const sine_inc = audio_dsp_phase_inc(f);

        float s = audio_dsp_sin(st->sine_phase) * BUMP_AMP_SINE;
        st->sine_phase += sine_inc;

        float n = audio_dsp_noise(&st->noise_state);
        n = audio_biquad_tick(&st->lpf, n);
        n *= BUMP_AMP_NOISE;

        float const mix = (s + n) * env;
        int16_t const sample = audio_dsp_to_s16(mix);
        out[2 * i + 0] = sample;
        out[2 * i + 1] = sample;

        st->elapsed_samples++;
    }

    if (audio_env_is_idle(&st->env)) {
        self->finished = true;
    }
}

static void bump_shutdown(sfx_voice_t* self) {
    heap_caps_free(self);
}

bool sfx_cube_bump_play(void) {
    bump_state_t* st = (bump_state_t*)heap_caps_calloc(1, sizeof(*st), MALLOC_CAP_INTERNAL);
    if (st == NULL) {
        ESP_LOGW(TAG, "alloc failed");
        return false;
    }

    st->voice.render   = bump_render;
    st->voice.shutdown = bump_shutdown;
    st->voice.finished = false;
    st->noise_state    = 0xBEEFFACEu;
    st->total_samples  = (uint32_t)((BUMP_ATTACK_S + BUMP_DECAY_S) * (float)AUDIO_SAMPLE_RATE_HZ);
    audio_biquad_lpf(&st->lpf, BUMP_NOISE_LPF_HZ, 0.7f);
    audio_env_configure(&st->env, BUMP_ATTACK_S, BUMP_DECAY_S, 0.0f, 0.001f);
    audio_env_trigger(&st->env);

    if (!audio_mixer_register_voice(&st->voice)) {
        heap_caps_free(st);
        return false;
    }
    return true;
}
