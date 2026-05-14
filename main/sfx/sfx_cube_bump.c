#include "sfx/sfx_cube_bump.h"

#include "audio_dsp.h"
#include "audio_mixer.h"
#include "audio_source.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdint.h>
#include <string.h>

static char const TAG[] = "sfx_bump";

// Kick-drum-style "wump" for the flipping cube landing flush.
//
// **Speaker-vs-headphone caveat.** The Tanmatsu's built-in speaker
// is a tiny chip-driven driver that rolls off hard below ~200 Hz —
// at 55 Hz almost nothing comes through. Headphones reproduce the
// full bass range. To make the cube hit feel weighty on *both*
// outputs we rely on the **missing-fundamental psychoacoustic
// effect**: if the listener hears the harmonic series (2×, 3×, 4×
// frequency) of a low fundamental, the brain reconstructs the
// fundamental even when the speaker can't physically reproduce it.
// Phone and laptop speakers fake their entire bass response this
// way.
//
// Voice layers:
//   1. **Fundamental** (BUMP_F_HIGH → BUMP_F_LOW sweep): sine
//      pitched from a higher start to a sustained low end. Settles
//      in ~90 ms then rings out at the low frequency. Audible on
//      headphones, mostly filtered by the speaker.
//   2. **2nd harmonic** (2× fundamental): tracks the swept
//      fundamental at double the phase rate. Sits in the
//      110–380 Hz band over the sweep — fully reproduced by the
//      built-in speaker, and the dominant contributor to the
//      missing-fundamental illusion.
//   3. **3rd harmonic** (3× fundamental): adds upper midrange
//      presence (165–570 Hz). Speaker-audible. Combined with the
//      2nd harmonic, the brain "fills in" the fundamental pitch.
//   4. **Sub-octave** (½ fundamental): adds chest-thump on
//      headphones. Inaudible on speaker but cheap to keep — kept
//      low so it doesn't make the headphone mix muddy.
//   5. **Click transient**: short (~8 ms) lowpassed noise burst
//      with its own envelope — sells the impact moment without
//      dominating the tonal body.
//
// Decoupled envelopes for the body and the click let us shape
// each independently: short fast click, long musical body.

// Fundamental sweep: BUMP_F_HIGH at t=0, asymptoting to BUMP_F_LOW
// along a (1 - (1-t)^4) curve so the pitch is 94% of the way down
// by t≈0.5 of the SWEEP duration. The sweep duration is
// deliberately **independent** of the body envelope length —
// kicks settle on their fundamental in ≲100 ms then sustain at
// the low frequency for the much longer tail. Tying the two
// together (previous version) turned the pitch drop into a slow
// glide which read as "click" rather than "boom".
#define BUMP_F_HIGH_HZ      190.0f
#define BUMP_F_LOW_HZ        55.0f
#define BUMP_SWEEP_S          0.09f

// Body envelope: very fast attack so the impact is snappy, long
// decay so the low tail rings out for ~700 ms — that's where the
// "big and heavy" perception lives, well past the pitch sweep.
// Sustain = 0 so the envelope drops to IDLE when decay completes.
#define BUMP_BODY_ATTACK_S    0.0015f
#define BUMP_BODY_DECAY_S     0.70f

// Click envelope: short, just enough to sell the impact transient
// before the tonal body takes over. Brighter than before so it
// reads as the "pop" of the impact rather than as low rumble noise.
#define BUMP_CLICK_ATTACK_S   0.0005f
#define BUMP_CLICK_DECAY_S    0.008f

// Per-layer nominal amplitudes. These set the *relative balance*
// between the tonal body, the harmonic series that carries the
// bass on the built-in speaker, the sub-octave thickener
// (headphones only), and the click pop.
//
// The 2nd and 3rd harmonics together carry the apparent bass on
// the speaker via the missing-fundamental effect, so they're
// proportionally substantial — not so loud that they dominate
// on headphones (where the fundamental is doing the work) but
// loud enough to be the speaker's primary cue. Sum of all
// layers (worst-case in-phase peak) is ~1.79.
//
// BUMP_VOICE_GAIN compresses the whole multi-layer voice into a
// single-voice budget that fits the mixer's 5-concurrent-SFX
// headroom math (≈ 0.5 nominal per voice → 0.18 effective after
// the SFX master gain). Without it, dynamic_passage areas —
// which can fire 4–7 cube-bump voices within ~700 ms (the body
// decay length) of each other — would push the accumulator past
// the hard-clip ceiling. Tuned 2026-05-14 after observing actual
// clipping in dynamic_passage playback.
#define BUMP_AMP_FUNDAMENTAL  0.70f
#define BUMP_AMP_HARMONIC_2   0.40f
#define BUMP_AMP_HARMONIC_3   0.22f
#define BUMP_AMP_SUB          0.25f
#define BUMP_AMP_CLICK        0.22f
#define BUMP_VOICE_GAIN       0.40f

// Lowpass cutoff for the click — keep it midrange-y so it reads
// as "thud impact pop", not "hi-hat tick".
#define BUMP_CLICK_LPF_HZ   1600.0f

typedef struct {
    sfx_voice_t    voice;
    audio_env_t    body_env;
    audio_env_t    click_env;
    audio_biquad_t click_lpf;
    // Phase accumulators for each tonal layer. All track the same
    // swept melody at their respective frequency multiples — the
    // 2nd harmonic is advanced at 2× the fundamental's phase
    // increment per sample, etc. — so they all stay in their
    // proper harmonic relationship through the pitch sweep.
    uint32_t       fund_phase;
    uint32_t       h2_phase;
    uint32_t       h3_phase;
    uint32_t       sub_phase;
    uint32_t       noise_state;
    uint32_t       elapsed_samples;
    // Sample at which the pitch sweep concludes. Past this point
    // the fundamental holds at BUMP_F_LOW_HZ for the rest of the
    // body envelope's tail.
    uint32_t       sweep_end_sample;
} bump_state_t;

// Pitch-sweep curve: returns the lerp coefficient (0..1) at the
// given elapsed-time normalised position. 1 - (1-t)^4 starts steep
// and flattens out: ~0.59 at t=0.2, ~0.94 at t=0.5, ~0.999 at t=0.9.
// This is what makes the kick "settle" on its fundamental quickly
// rather than smoothly gliding down for the full duration.
static float sweep_curve(float t) {
    if (t < 0.0f) return 0.0f;
    if (t > 1.0f) return 1.0f;
    float const u = 1.0f - t;
    return 1.0f - (u * u * u * u);
}

static float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

static void bump_render(sfx_voice_t* self, int16_t* out, size_t frames) {
    bump_state_t* st = (bump_state_t*)self;

    for (size_t i = 0; i < frames; i++) {
        float const body_env  = audio_env_tick(&st->body_env);
        float const click_env = audio_env_tick(&st->click_env);

        // Pitch sweep position. Computed against the *sweep*
        // sample count, not the envelope length — the pitch
        // settles on BUMP_F_LOW_HZ within ~90 ms, then the body
        // envelope keeps ringing out at the low frequency for
        // the rest of the (much longer) tail. That long sustained
        // low tail is what gives the wump its "big and heavy"
        // feel; tying the sweep to the envelope length stretches
        // the pitch drop into a glide and the cue stops reading
        // as percussive.
        float t = (st->sweep_end_sample > 0)
                  ? (float)st->elapsed_samples / (float)st->sweep_end_sample
                  : 1.0f;
        if (t > 1.0f) t = 1.0f;
        float const f_fund   = lerpf(BUMP_F_HIGH_HZ, BUMP_F_LOW_HZ, sweep_curve(t));
        uint32_t const inc_f = audio_dsp_phase_inc(f_fund);
        // Harmonic / sub phase increments are exact multiples of
        // the fundamental's. Cheap, perfectly phase-locked across
        // the sweep — the harmonic stack tracks the fundamental's
        // pitch contour without any extra math.
        uint32_t const inc_h2 = inc_f << 1;   // 2× fundamental
        uint32_t const inc_h3 = inc_f + (inc_f << 1);  // 3× fundamental
        uint32_t const inc_sub = inc_f >> 1;  // 0.5× fundamental

        float const fund = audio_dsp_sin(st->fund_phase) * BUMP_AMP_FUNDAMENTAL;
        float const h2   = audio_dsp_sin(st->h2_phase)   * BUMP_AMP_HARMONIC_2;
        float const h3   = audio_dsp_sin(st->h3_phase)   * BUMP_AMP_HARMONIC_3;
        float const sub  = audio_dsp_sin(st->sub_phase)  * BUMP_AMP_SUB;
        float       click = audio_dsp_noise(&st->noise_state);
        click = audio_biquad_tick(&st->click_lpf, click) * BUMP_AMP_CLICK;

        st->fund_phase += inc_f;
        st->h2_phase   += inc_h2;
        st->h3_phase   += inc_h3;
        st->sub_phase  += inc_sub;

        float const mix = ((fund + h2 + h3 + sub) * body_env + click * click_env)
                          * BUMP_VOICE_GAIN;
        int16_t const sample = audio_dsp_to_s16(mix);
        out[2 * i + 0] = sample;
        out[2 * i + 1] = sample;

        st->elapsed_samples++;
    }

    if (audio_env_is_idle(&st->body_env)) {
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

    st->voice.render        = bump_render;
    st->voice.shutdown      = bump_shutdown;
    st->voice.finished      = false;
    st->noise_state         = 0xBEEFFACEu;
    st->sweep_end_sample    = (uint32_t)(BUMP_SWEEP_S * (float)AUDIO_SAMPLE_RATE_HZ);

    audio_biquad_lpf(&st->click_lpf, BUMP_CLICK_LPF_HZ, 0.7f);
    audio_env_configure(&st->body_env,  BUMP_BODY_ATTACK_S,  BUMP_BODY_DECAY_S,  0.0f, 0.001f);
    audio_env_configure(&st->click_env, BUMP_CLICK_ATTACK_S, BUMP_CLICK_DECAY_S, 0.0f, 0.001f);
    audio_env_trigger(&st->body_env);
    audio_env_trigger(&st->click_env);

    if (!audio_mixer_register_voice(&st->voice)) {
        heap_caps_free(st);
        return false;
    }
    return true;
}
