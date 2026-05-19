#include "sfx/sfx_gong.h"

#include "audio_dsp.h"
#include "audio_mixer.h"
#include "audio_source.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdint.h>
#include <stddef.h>

static char const TAG[] = "sfx_gong";

// Struck-gong partial stack. The fundamental sits low; the upper
// partials mix harmonic ratios (2, 4) with slightly-inharmonic ones
// (2.03, 2.76, 5.43) for the metallic shimmer. Partials 1 and 2 are
// a hair apart (2.00 / 2.03) so they beat against each other — that
// slow throb is the gong "bloom". Higher partials decay faster, as
// real struck metal does.
#define GONG_PARTIALS  6
#define GONG_F0        196.0f          // ~G3 fundamental

static float const GONG_RATIO[GONG_PARTIALS] = { 1.00f, 2.00f, 2.03f, 2.76f, 4.10f, 5.43f };
static float const GONG_AMP_R[GONG_PARTIALS] = { 1.00f, 0.60f, 0.45f, 0.40f, 0.25f, 0.18f };
static float const GONG_DECAY[GONG_PARTIALS] = { 2.20f, 1.80f, 1.80f, 1.20f, 0.90f, 0.60f };

#define GONG_ATTACK_S  0.004f          // fast strike
#define GONG_AMP       0.42f           // per-voice nominal level

typedef struct {
    sfx_voice_t voice;
    uint32_t    phase[GONG_PARTIALS];
    audio_env_t env[GONG_PARTIALS];
} gong_state_t;

static void gong_render(sfx_voice_t* self, int16_t* out, size_t frames) {
    gong_state_t* st = (gong_state_t*)self;

    uint32_t inc[GONG_PARTIALS];
    for (int p = 0; p < GONG_PARTIALS; p++) {
        inc[p] = audio_dsp_phase_inc(GONG_F0 * GONG_RATIO[p]);
    }

    for (size_t i = 0; i < frames; i++) {
        float s = 0.0f;
        for (int p = 0; p < GONG_PARTIALS; p++) {
            float const e = audio_env_tick(&st->env[p]);
            s += audio_dsp_sin(st->phase[p]) * e * GONG_AMP_R[p];
            st->phase[p] += inc[p];
        }
        s *= GONG_AMP;

        int16_t const sample = audio_dsp_to_s16(s);
        out[2 * i + 0] = sample;
        out[2 * i + 1] = sample;
    }

    // Done once every partial's envelope has decayed back to idle.
    bool all_idle = true;
    for (int p = 0; p < GONG_PARTIALS; p++) {
        if (!audio_env_is_idle(&st->env[p])) { all_idle = false; break; }
    }
    if (all_idle) self->finished = true;
}

static void gong_shutdown(sfx_voice_t* self) {
    heap_caps_free(self);
}

bool sfx_gong_play(void) {
    gong_state_t* st = (gong_state_t*)heap_caps_calloc(1, sizeof(*st), MALLOC_CAP_INTERNAL);
    if (st == NULL) {
        ESP_LOGW(TAG, "alloc failed");
        return false;
    }

    st->voice.render   = gong_render;
    st->voice.shutdown = gong_shutdown;
    st->voice.finished = false;

    for (int p = 0; p < GONG_PARTIALS; p++) {
        // sustain=0 → clean attack-decay one-shot ending in IDLE.
        audio_env_configure(&st->env[p], GONG_ATTACK_S, GONG_DECAY[p], 0.0f, 0.001f);
        audio_env_trigger(&st->env[p]);
    }

    if (!audio_mixer_register_voice(&st->voice)) {
        heap_caps_free(st);
        return false;
    }
    return true;
}
