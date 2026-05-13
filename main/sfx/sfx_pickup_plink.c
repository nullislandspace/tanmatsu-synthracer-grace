#include "sfx/sfx_pickup_plink.h"

#include "audio_dsp.h"
#include "audio_mixer.h"
#include "audio_source.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdint.h>
#include <string.h>

static char const TAG[] = "sfx_plink";

// C-major pentatonic, ascending. Picks 1..5 within a multiplier
// cycle map to slot indices 0..4 in this table. The 5th note (A5)
// plays as the multiplier ticks up and the HUD progress row
// resets — it's the audio cue for the multiplier bump.
//
// Frequencies are equal-tempered MIDI: C5=72, D5=74, E5=76,
// G5=79, A5=81. f = 440 × 2^((midi-69)/12). Hardcoded so we
// don't need expf at runtime.
static float const PLINK_FREQS_HZ[5] = {
    523.25f,   // C5
    587.33f,   // D5
    659.26f,   // E5
    783.99f,   // G5
    880.00f,   // A5
};

// Bell-like single sine pulse: very fast attack, modest decay,
// no noise component. The slight ringing length is what makes it
// read as "musical chime" rather than "percussion hit".
#define PLINK_ATTACK_S 0.003f
#define PLINK_DECAY_S  0.14f

// Per-voice nominal amplitude. Scaled by the SFX master gain
// (AUDIO_SFX_GAIN) at the mixer; effective peak per voice is
// the product. Roughly matches the booster's existing ding so
// the two pickup cues sit at the same loudness.
#define PLINK_AMP 0.50f

typedef struct {
    sfx_voice_t voice;
    uint32_t    phase;
    uint32_t    phase_inc;
    audio_env_t env;
} plink_state_t;

static void plink_render(sfx_voice_t* self, int16_t* out, size_t frames) {
    plink_state_t* st = (plink_state_t*)self;

    for (size_t i = 0; i < frames; i++) {
        float const e  = audio_env_tick(&st->env);
        float       s  = audio_dsp_sin(st->phase) * e * PLINK_AMP;
        st->phase     += st->phase_inc;

        int16_t const sample = audio_dsp_to_s16(s);
        out[2 * i + 0] = sample;
        out[2 * i + 1] = sample;
    }

    if (audio_env_is_idle(&st->env)) {
        self->finished = true;
    }
}

static void plink_shutdown(sfx_voice_t* self) {
    heap_caps_free(self);
}

bool sfx_pickup_plink_play(int slot_index) {
    if (slot_index < 0)  slot_index = 0;
    if (slot_index > 4)  slot_index = 4;

    plink_state_t* st = (plink_state_t*)heap_caps_calloc(1, sizeof(*st), MALLOC_CAP_INTERNAL);
    if (st == NULL) {
        ESP_LOGW(TAG, "alloc failed");
        return false;
    }

    st->voice.render   = plink_render;
    st->voice.shutdown = plink_shutdown;
    st->voice.finished = false;
    st->phase          = 0;
    st->phase_inc      = audio_dsp_phase_inc(PLINK_FREQS_HZ[slot_index]);

    // sustain=0 → envelope is a clean attack-decay one-shot that
    // transitions to IDLE on its own when the decay completes.
    audio_env_configure(&st->env, PLINK_ATTACK_S, PLINK_DECAY_S, 0.0f, 0.001f);
    audio_env_trigger(&st->env);

    if (!audio_mixer_register_voice(&st->voice)) {
        heap_caps_free(st);
        return false;
    }
    return true;
}
