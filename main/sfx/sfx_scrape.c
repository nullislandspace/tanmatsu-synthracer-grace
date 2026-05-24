#include "sfx/sfx_scrape.h"

#include "se_audio_dsp.h"
#include "se_audio.h"
#include "se_audio_source.h"

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

// Bandpassed noise with a slow LFO on the centre frequency gives
// the shimmering quality of metal-on-metal. Intensity boosts
// amplitude and pulls the bandpass towards higher / more
// resonant settings.

#define SCRAPE_BPF_MIN_HZ  900.0f
#define SCRAPE_BPF_MAX_HZ 2800.0f
#define SCRAPE_BPF_LFO_HZ    7.0f
#define SCRAPE_BPF_LFO_DEPTH 350.0f

#define SCRAPE_Q_MIN 0.6f
#define SCRAPE_Q_MAX 3.5f

// Per-voice nominal amplitude (max — actual amp scales with
// the intensity driver). Scrape can run concurrently with up
// to four other one-shots; the SFX master gain takes care of
// the overall headroom budget.
#define SCRAPE_AMP_MAX 0.40f

typedef struct {
    sfx_voice_t   voice;
    uint32_t      noise_state;
    audio_biquad_t bpf;
    uint32_t      lfo_phase;
    _Atomic float intensity;  // 0..1 — driver from game thread
} scrape_state_t;

static scrape_state_t s_scrape;
static bool           s_running = false;

static float lerpf(float a, float b, float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return a + (b - a) * t;
}

static void scrape_render(sfx_voice_t* self, int16_t* out, size_t frames) {
    scrape_state_t* st = (scrape_state_t*)self;

    float const intensity = atomic_load(&st->intensity);

    float const fc_base = lerpf(SCRAPE_BPF_MIN_HZ, SCRAPE_BPF_MAX_HZ, intensity);
    float const q       = lerpf(SCRAPE_Q_MIN, SCRAPE_Q_MAX, intensity);
    float const amp     = SCRAPE_AMP_MAX * intensity;

    // Update filter once per chunk — cheaper than per-sample and
    // the LFO is slow enough that audible "stepping" is masked
    // by the noise.
    float const lfo  = audio_dsp_sin(st->lfo_phase);
    float const fc   = fc_base + lfo * SCRAPE_BPF_LFO_DEPTH;
    audio_biquad_bpf(&st->bpf, fc, q);
    st->lfo_phase += audio_dsp_phase_inc(SCRAPE_BPF_LFO_HZ);

    for (size_t i = 0; i < frames; i++) {
        float n = audio_dsp_noise(&st->noise_state);
        n = audio_biquad_tick(&st->bpf, n);
        n *= amp;

        int16_t const sample = audio_dsp_to_s16(n);
        out[2 * i + 0] = sample;
        out[2 * i + 1] = sample;
    }
}

bool sfx_scrape_start(void) {
    if (s_running) return true;

    memset(&s_scrape, 0, sizeof(s_scrape));
    s_scrape.voice.render   = scrape_render;
    s_scrape.voice.shutdown = NULL;
    s_scrape.voice.finished = false;
    s_scrape.noise_state    = 0xA5A5BEEFu;
    audio_biquad_bpf(&s_scrape.bpf, SCRAPE_BPF_MIN_HZ, SCRAPE_Q_MIN);
    atomic_store(&s_scrape.intensity, 0.0f);

    if (!audio_mixer_register_voice(&s_scrape.voice)) return false;
    s_running = true;
    return true;
}

void sfx_scrape_stop(void) {
    if (!s_running) return;
    audio_mixer_stop_voice(&s_scrape.voice);
    s_running = false;
}

void sfx_scrape_set_intensity(float intensity) {
    if (intensity < 0.0f) intensity = 0.0f;
    if (intensity > 1.0f) intensity = 1.0f;
    atomic_store(&s_scrape.intensity, intensity);
}
