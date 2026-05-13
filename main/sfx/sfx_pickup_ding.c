#include "sfx/sfx_pickup_ding.h"

#include "audio_dsp.h"
#include "audio_mixer.h"
#include "audio_source.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdint.h>
#include <string.h>

static char const TAG[] = "sfx_ding";

// Two-tone "ding-dong" shape. The two pitches are a major third
// apart (5:4 ratio) — sounds bell-like without being too saccharine.
#define DING_F1 1320.0f          // ~E6
#define DING_F2 (DING_F1 * (5.0f / 4.0f))  // ~G#6

// Each note: short attack, longer decay. We use two envelopes
// because the second tone starts later.
#define DING_NOTE_ATTACK_S 0.005f
#define DING_NOTE_DECAY_S  0.18f
#define DING_NOTE_GAP_S    0.08f

// Per-voice nominal amplitude. The SFX master gain
// (AUDIO_SFX_GAIN) scales this down at mix-down time, so this
// value sets relative loudness against other one-shots and the
// magicnumbers gain handles the overall bus level + headroom
// for 5 concurrent voices.
#define DING_AMP 0.50f

typedef struct {
    sfx_voice_t voice;
    uint32_t    phase1;
    uint32_t    phase2;
    audio_env_t env1;
    audio_env_t env2;
    uint32_t    samples_until_note2;
    uint32_t    elapsed_samples;
} ding_state_t;

static void ding_render(sfx_voice_t* self, int16_t* out, size_t frames) {
    ding_state_t* st = (ding_state_t*)self;

    uint32_t const inc1 = audio_dsp_phase_inc(DING_F1);
    uint32_t const inc2 = audio_dsp_phase_inc(DING_F2);

    for (size_t i = 0; i < frames; i++) {
        if (st->samples_until_note2 > 0) {
            st->samples_until_note2--;
            if (st->samples_until_note2 == 0) {
                audio_env_trigger(&st->env2);
            }
        }

        float const e1 = audio_env_tick(&st->env1);
        float const e2 = audio_env_tick(&st->env2);
        float       s  = audio_dsp_sin(st->phase1) * e1 + audio_dsp_sin(st->phase2) * e2;
        s *= DING_AMP;

        int16_t const sample = audio_dsp_to_s16(s);
        out[2 * i + 0] = sample;
        out[2 * i + 1] = sample;

        st->phase1 += inc1;
        st->phase2 += inc2;
        st->elapsed_samples++;
    }

    // Done when both envelopes have decayed back to idle and the
    // second note has actually been triggered.
    if (st->samples_until_note2 == 0
        && audio_env_is_idle(&st->env1)
        && audio_env_is_idle(&st->env2)) {
        self->finished = true;
    }
}

static void ding_shutdown(sfx_voice_t* self) {
    heap_caps_free(self);
}

bool sfx_pickup_ding_play(void) {
    ding_state_t* st = (ding_state_t*)heap_caps_calloc(1, sizeof(*st), MALLOC_CAP_INTERNAL);
    if (st == NULL) {
        ESP_LOGW(TAG, "alloc failed");
        return false;
    }

    st->voice.render   = ding_render;
    st->voice.shutdown = ding_shutdown;
    st->voice.finished = false;

    // sustain=0 → envelope is a clean attack-decay one-shot that
    // transitions to IDLE when decay reaches the floor.
    audio_env_configure(&st->env1, DING_NOTE_ATTACK_S, DING_NOTE_DECAY_S, 0.0f, 0.001f);
    audio_env_configure(&st->env2, DING_NOTE_ATTACK_S, DING_NOTE_DECAY_S, 0.0f, 0.001f);
    audio_env_trigger(&st->env1);
    st->samples_until_note2 = (uint32_t)(DING_NOTE_GAP_S * (float)AUDIO_SAMPLE_RATE_HZ);

    if (!audio_mixer_register_voice(&st->voice)) {
        heap_caps_free(st);
        return false;
    }
    return true;
}
