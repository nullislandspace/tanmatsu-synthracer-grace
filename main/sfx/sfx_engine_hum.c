#include "sfx/sfx_engine_hum.h"

#include "audio_dsp.h"
#include "audio_mixer.h"
#include "audio_source.h"

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

// Frequency range, Hz. The two detuned saws are placed around
// this centre at ±DETUNE_HZ for a thicker sound.
#define HUM_F_MIN   55.0f
#define HUM_F_MAX  220.0f
#define HUM_DETUNE_HZ 0.6f

// Master amplitude relative to int16 full-scale. Engine hum sits
// underneath the rest of the soundscape; ~10% is plenty.
#define HUM_AMP 0.10f

// Lowpass cutoff, also scaled with speed so the hum opens up
// when accelerating.
#define HUM_LPF_MIN_HZ  300.0f
#define HUM_LPF_MAX_HZ 1800.0f

typedef struct {
    sfx_voice_t voice;
    uint32_t    phase_a;
    uint32_t    phase_b;
    audio_biquad_t lpf;
    // Driver value updated from the game thread, read by the
    // mixer task at the start of each chunk. `_Atomic` for
    // cross-core visibility without locking.
    _Atomic float speed_norm;
} hum_state_t;

static hum_state_t s_hum;
static bool        s_running = false;

static float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

static void hum_render(sfx_voice_t* self, int16_t* out, size_t frames) {
    hum_state_t* st = (hum_state_t*)self;

    float speed = atomic_load(&st->speed_norm);
    if (speed < 0.0f) speed = 0.0f;
    if (speed > 1.0f) speed = 1.0f;

    float const base_f = lerpf(HUM_F_MIN, HUM_F_MAX, speed);
    uint32_t const inc_a = audio_dsp_phase_inc(base_f - HUM_DETUNE_HZ);
    uint32_t const inc_b = audio_dsp_phase_inc(base_f + HUM_DETUNE_HZ);

    float const fc = lerpf(HUM_LPF_MIN_HZ, HUM_LPF_MAX_HZ, speed);
    audio_biquad_lpf(&st->lpf, fc, 0.8f);

    for (size_t i = 0; i < frames; i++) {
        float const a = audio_dsp_saw(st->phase_a);
        float const b = audio_dsp_saw(st->phase_b);
        float       s = (a + b) * 0.5f;
        s = audio_biquad_tick(&st->lpf, s);
        s *= HUM_AMP;

        int16_t const sample = audio_dsp_to_s16(s);
        out[2 * i + 0] = sample;
        out[2 * i + 1] = sample;

        st->phase_a += inc_a;
        st->phase_b += inc_b;
    }
}

bool sfx_engine_hum_start(void) {
    if (s_running) return true;

    memset(&s_hum, 0, sizeof(s_hum));
    s_hum.voice.render   = hum_render;
    s_hum.voice.shutdown = NULL;
    s_hum.voice.finished = false;
    s_hum.phase_a        = 0;
    s_hum.phase_b        = 0x40000000u;  // 90° offset for stereo width / detune
    audio_biquad_lpf(&s_hum.lpf, HUM_LPF_MIN_HZ, 0.8f);
    atomic_store(&s_hum.speed_norm, 0.0f);

    if (!audio_mixer_register_voice(&s_hum.voice)) return false;
    s_running = true;
    return true;
}

void sfx_engine_hum_stop(void) {
    if (!s_running) return;
    audio_mixer_stop_voice(&s_hum.voice);
    s_running = false;
}

void sfx_engine_hum_set_pitch(float speed_normalised) {
    atomic_store(&s_hum.speed_norm, speed_normalised);
}
