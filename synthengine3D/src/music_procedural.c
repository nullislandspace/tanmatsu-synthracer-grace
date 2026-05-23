#include "se_music_procedural.h"

#include "se_audio_dsp.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static char const TAG[] = "music_proc";

// ---------------------------------------------------------------
// Musical constants
// ---------------------------------------------------------------

// Synthwave sits around 110 BPM (Kavinsky, Mitch Murder, "Drive"
// soundtrack territory). Rather than lock every run to the same
// metronome, each run picks an integer BPM in [100, 118]; the
// per-16th-note sample count is derived once per run into
// music_proc_t::samples_per_16th. A double accumulator keeps drift
// over a long run sub-sample.
#define MUSIC_BPM_MIN   100u
#define MUSIC_BPM_SPAN   19u   // → 100..118 BPM inclusive

// 16 sixteenths per bar (4/4 time), 2 bars per chord, 8 chords per
// progression ⇒ 16 bars per section.
#define TICKS_PER_BAR     16
#define BARS_PER_CHORD     2
#define CHORDS_PER_SECTION 8
#define BARS_PER_SECTION   (CHORDS_PER_SECTION * BARS_PER_CHORD)

// Layer master gains (relative to int16 full-scale, before the
// mixer's music-gain pass at ~30%). Summed, they comfortably fit
// under 1.0 so the soft-clipper doesn't fire.
#define BASS_AMP   0.30f
#define ARP_AMP    0.16f
#define PAD_AMP    0.14f
#define KICK_AMP   0.45f
#define SNARE_AMP  0.32f
#define HAT_AMP    0.10f

// ---------------------------------------------------------------
// Chord bank — eight 16-bar progressions in natural minor.
// `root_offset` is semitones above the run's tonic; `is_major`
// picks between minor (third=+3) and major (third=+4) triad.
// ---------------------------------------------------------------

typedef struct {
    int8_t  root_offset;
    uint8_t is_major;
} chord_t;

// Scale-degree shortcuts for natural minor.
#define i_   {0, 0}
#define III_ {3, 1}
#define iv_  {5, 0}
#define v_   {7, 0}
#define V_   {7, 1}   // Picardy v → V for tension lifts
#define VI_  {8, 1}
#define VII_ {10, 1}

typedef struct {
    chord_t slots[CHORDS_PER_SECTION];
} progression_t;

static progression_t const g_progressions[] = {
    // i – VII – VI – VII  ("Drive")
    { { i_, VII_, VI_, VII_, i_, VII_, VI_, VII_ } },
    // i – VI – III – VII  ("Outrun" classic)
    { { i_, VI_, III_, VII_, i_, VI_, III_, VII_ } },
    // i – v  – VI – VII
    { { i_, v_, VI_, VII_, i_, v_, VI_, VII_ } },
    // i – iv – VII – III
    { { i_, iv_, VII_, III_, i_, iv_, VII_, III_ } },
    // i – VII – v – VI
    { { i_, VII_, v_, VI_, i_, VII_, v_, VI_ } },
    // VI – VII – i – i (climbing rise)
    { { VI_, VII_, i_, i_, VI_, VII_, i_, i_ } },
    // i – VI – iv – V (synthwave power-pop)
    { { i_, VI_, iv_, V_, i_, VI_, iv_, V_ } },
    // i – III – VII – v
    { { i_, III_, VII_, v_, i_, III_, VII_, v_ } },
};
#define NUM_PROGRESSIONS (sizeof(g_progressions) / sizeof(g_progressions[0]))

// Arp pattern bank. Each pattern is one bar of 16 sixteenths; the
// chord holds for two bars so a pattern plays twice per chord. Step
// values index the chord triad lifted into octaves:
//   0 root    1 third    2 fifth
//   3 root+8  4 third+8  5 fifth+8
// and -1 means "rest" (no note this 16th) for rhythmic variety. A
// fresh pattern is chosen per 16-bar section.
typedef struct {
    int8_t step[TICKS_PER_BAR];
} arp_pattern_t;

static arp_pattern_t const g_arp_patterns[] = {
    // 0: the classic up-down noodle
    { { 0, 2, 1, 2, 3, 2, 1, 2,  0, 2, 1, 2, 3, 2, 1, 2 } },
    // 1: sparse ascending run, rests on every off-16th
    { { 0,-1, 1,-1, 2,-1, 3,-1,  4,-1, 3,-1, 2,-1, 1,-1 } },
    // 2: octave bounce — a tone then its octave, climbing
    { { 0, 3, 0, 3, 1, 4, 1, 4,  2, 5, 2, 5, 1, 4, 1, 4 } },
    // 3: syncopated and sparse — leaves big gaps for the pad
    { { 0,-1,-1, 2, 1,-1, 3,-1,  0,-1, 2,-1,-1, 3, 1,-1 } },
    // 4: straight 1-3-5-8 climb and fall
    { { 0, 1, 2, 3, 4, 3, 2, 1,  0, 1, 2, 3, 4, 5, 4, 3 } },
    // 5: pulsing root with melodic accents on the off-beats
    { { 0, 0, 2, 0, 1, 0, 3, 0,  0, 0, 2, 0, 1, 0, 4, 0 } },
};
#define NUM_ARP_PATTERNS (sizeof(g_arp_patterns) / sizeof(g_arp_patterns[0]))

// Drum pattern bank — each is 16 booleans (one per 16th in a bar).
typedef struct {
    uint16_t kick;
    uint16_t snare;
    uint16_t hat;
} drum_pattern_t;

// Bit positions are 16th-indexed (LSB = beat 1, 1st 16th).
static drum_pattern_t const g_drum_patterns[] = {
    // 0: classic four-on-the-floor with backbeat snare
    {
        .kick  = 0x0101u,                    // 16ths 0, 8
        .snare = 0x1010u,                    // 16ths 4, 12
        .hat   = 0x5555u,                    // every 8th (16ths 0,2,4,6,8,10,12,14)
    },
    // 1: pumping eighths on kick
    {
        .kick  = 0x5555u,
        .snare = 0x1010u,
        .hat   = 0xAAAAu,                    // every off-8th
    },
    // 2: half-time feel
    {
        .kick  = 0x0001u,                    // only on beat 1
        .snare = 0x0100u,                    // on beat 3
        .hat   = 0x5555u,
    },
    // 3: driving — quarter-note kick under busy 16th hats
    {
        .kick  = 0x1111u,                    // 16ths 0,4,8,12
        .snare = 0x1010u,                    // 16ths 4,12
        .hat   = 0xFFFFu,                    // every 16th
    },
    // 4: broken kick with a syncopated push before beat 3
    {
        .kick  = 0x0411u,                    // 16ths 0,4,10
        .snare = 0x1010u,
        .hat   = 0x5555u,
    },
    // 5: sparse and airy — half-time kick, offbeat hats only
    {
        .kick  = 0x0101u,                    // 16ths 0,8
        .snare = 0x0100u,                    // beat 3 only
        .hat   = 0xAAAAu,                    // every off-8th
    },
};
#define NUM_DRUM_PATTERNS (sizeof(g_drum_patterns) / sizeof(g_drum_patterns[0]))

// Bass rhythm bank — 16-bit masks, one bit per 16th (LSB = 16th 0).
// On each set bit the bass pulses the chord root an octave down. A
// pattern is chosen per section so the low-end drive shifts with
// the music.
static uint16_t const g_bass_patterns[] = {
    0x5555u,   // 0: steady eighth-note pulse
    0xFFFFu,   // 1: driving sixteenths — the "outrun chase" feel
    0x1111u,   // 2: laid-back quarter notes, lets the pad breathe
    0x9999u,   // 3: galloping — beat plus the 16th before the next
};
#define NUM_BASS_PATTERNS (sizeof(g_bass_patterns) / sizeof(g_bass_patterns[0]))

// ---------------------------------------------------------------
// PRNG (xorshift32) and a couple of helpers
// ---------------------------------------------------------------

static uint32_t prng_next(uint32_t* s) {
    uint32_t x = *s;
    if (x == 0u) x = 0x12345678u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}
static uint32_t prng_range(uint32_t* s, uint32_t n) {
    return n == 0 ? 0 : (prng_next(s) % n);
}

static float midi_to_hz(int midi) {
    return 440.0f * powf(2.0f, ((float)midi - 69.0f) / 12.0f);
}

// Get the three chord-tone MIDI notes for a chord_t, relative to
// `root_midi`. tone[0]=root, tone[1]=third, tone[2]=fifth.
static void chord_tones(chord_t c, int root_midi, int tones_out[3]) {
    int const root = root_midi + c.root_offset;
    int const third = root + (c.is_major ? 4 : 3);
    int const fifth = root + 7;
    tones_out[0] = root;
    tones_out[1] = third;
    tones_out[2] = fifth;
}

// ---------------------------------------------------------------
// Voice state
// ---------------------------------------------------------------

typedef struct {
    music_source_t base;

    uint32_t prng;

    // Tonal centre for the run.
    int     root_midi;

    // Timing.
    double  samples_per_16th;  // per-run, derived from the chosen BPM
    double  sample_pos;        // double for sub-sample drift accuracy
    double  next_tick_at;
    uint16_t tick_in_bar;      // 0..15
    uint16_t bar_in_section;   // 0..15
    uint32_t section_index;

    // Current section's progression + selected pattern set.
    progression_t  prog;
    drum_pattern_t drums;
    arp_pattern_t  arp;            // this section's arp pattern
    uint8_t        arp_index;      // its index in g_arp_patterns (no-repeat)
    int            arp_octave;     // 0 or +12 — per-section arp transpose
    uint16_t       bass_mask;      // this section's bass rhythm
    bool           layer_bass;
    bool           layer_arp;
    bool           layer_pad;
    bool           layer_drums;

    // Bass voice.
    uint32_t       bass_phase;
    audio_env_t    bass_env;
    audio_biquad_t bass_lpf;
    float          bass_freq;

    // Arp voice.
    uint32_t       arp_phase;
    audio_env_t    arp_env;
    audio_biquad_t arp_lpf;
    float          arp_freq;

    // Pad voice — two detuned saws.
    uint32_t       pad_phase_a;
    uint32_t       pad_phase_b;
    uint32_t       pad_phase_c;   // fifth on top
    audio_env_t    pad_env;
    audio_biquad_t pad_lpf;
    float          pad_freq_a;
    float          pad_freq_b;
    float          pad_freq_c;
    uint32_t       pad_lfo_phase;
    int8_t         pad_last_chord_index;

    // Drums.
    uint32_t       kick_phase;
    audio_env_t    kick_env;
    uint32_t       snare_noise;
    audio_env_t    snare_env;
    audio_biquad_t snare_bpf;
    uint32_t       hat_noise;
    audio_env_t    hat_env;
    audio_biquad_t hat_hpf;
} music_proc_t;

// ---------------------------------------------------------------
// Section / chord / tick logic
// ---------------------------------------------------------------

static void start_new_section(music_proc_t* m) {
    // Pick a fresh progression. Try to avoid an immediate repeat
    // (one re-roll is enough — pleasant cadence variation).
    progression_t const* candidate = &g_progressions[prng_range(&m->prng, NUM_PROGRESSIONS)];
    if (memcmp(candidate, &m->prog, sizeof(progression_t)) == 0) {
        candidate = &g_progressions[prng_range(&m->prng, NUM_PROGRESSIONS)];
    }
    m->prog = *candidate;

    // Drums: most sections keep the same pattern; ~25% switch.
    if ((prng_next(&m->prng) & 3u) == 0u) {
        m->drums = g_drum_patterns[prng_range(&m->prng, NUM_DRUM_PATTERNS)];
    }

    // Arp: a fresh pattern every section, with one re-roll to dodge
    // an immediate repeat. Roughly a third of sections also lift the
    // whole arp an octave for a brighter, sparklier passage.
    {
        uint8_t idx = (uint8_t)prng_range(&m->prng, NUM_ARP_PATTERNS);
        if (idx == m->arp_index) {
            idx = (uint8_t)prng_range(&m->prng, NUM_ARP_PATTERNS);
        }
        m->arp_index = idx;
        m->arp       = g_arp_patterns[idx];
    }
    m->arp_octave = ((prng_next(&m->prng) % 3u) == 0u) ? 12 : 0;

    // Bass: pick a rhythm for the section's low end.
    m->bass_mask = g_bass_patterns[prng_range(&m->prng, NUM_BASS_PATTERNS)];

    // Layer mutation — every section may drop or restore one of
    // the harmonic layers. Drums and bass stay through the whole
    // run by default; arp/pad come and go for dynamics.
    if ((prng_next(&m->prng) & 1u) == 0u) {
        m->layer_arp = !m->layer_arp;
    }
    if ((prng_next(&m->prng) & 3u) == 0u) {
        m->layer_pad = !m->layer_pad;
    }
    // Keep at least one harmonic layer alive — if we just turned
    // both arp and pad off, force pad back on.
    if (!m->layer_arp && !m->layer_pad) m->layer_pad = true;

    m->section_index++;
    m->pad_last_chord_index = -1;  // force pad to retrigger
}

static chord_t current_chord(music_proc_t const* m) {
    int const chord_index = (m->bar_in_section / BARS_PER_CHORD) % CHORDS_PER_SECTION;
    return m->prog.slots[chord_index];
}

// Fired at the start of each 16th-note. Handles bar/section
// rollovers and (re-)triggers per-tick voice events.
static void on_tick(music_proc_t* m) {
    chord_t const c = current_chord(m);
    int tones[3];
    chord_tones(c, m->root_midi, tones);

    // Bass: pulse the chord root an octave down on every 16th the
    // section's bass pattern marks. The last 16th of a bar has a
    // chance to walk up to the fifth instead — a small lead-in to
    // the next chord.
    if (m->layer_bass) {
        uint16_t const bit = (uint16_t)(1u << m->tick_in_bar);
        if (m->bass_mask & bit) {
            bool const walk = (m->tick_in_bar == TICKS_PER_BAR - 1)
                           && ((prng_next(&m->prng) & 3u) == 0u);
            int const bass_midi = (walk ? tones[2] : tones[0]) - 12;
            m->bass_freq = midi_to_hz(bass_midi);
            audio_env_trigger(&m->bass_env);
        }
    }

    // Arp: walk the section's arp pattern. A negative step is a
    // rest (no trigger); the rest are chord tones lifted into
    // octaves, optionally transposed up an octave for the section.
    if (m->layer_arp) {
        int const step = m->arp.step[m->tick_in_bar];
        if (step >= 0) {
            int midi;
            switch (step) {
                case 0:  midi = tones[0];      break;
                case 1:  midi = tones[1];      break;
                case 2:  midi = tones[2];      break;
                case 3:  midi = tones[0] + 12; break;
                case 4:  midi = tones[1] + 12; break;
                default: midi = tones[2] + 12; break;
            }
            m->arp_freq = midi_to_hz(midi + m->arp_octave);
            audio_env_trigger(&m->arp_env);
        }
    }

    // Pad: retrigger on chord changes only.
    int8_t const chord_index = (int8_t)((m->bar_in_section / BARS_PER_CHORD) % CHORDS_PER_SECTION);
    if (m->layer_pad && chord_index != m->pad_last_chord_index) {
        // Three voices: root, third, fifth, all in the mid-octave.
        m->pad_freq_a = midi_to_hz(tones[0]);
        m->pad_freq_b = midi_to_hz(tones[1]);
        m->pad_freq_c = midi_to_hz(tones[2]);
        audio_env_trigger(&m->pad_env);
        m->pad_last_chord_index = chord_index;
    }

    // Drums.
    if (m->layer_drums) {
        uint16_t const bit = (uint16_t)(1u << m->tick_in_bar);
        if (m->drums.kick & bit) {
            audio_env_trigger(&m->kick_env);
            m->kick_phase = 0;
        }
        if (m->drums.snare & bit) {
            audio_env_trigger(&m->snare_env);
        }
        if (m->drums.hat & bit) {
            audio_env_trigger(&m->hat_env);
        }
    }

    // Advance bar / section counters.
    m->tick_in_bar++;
    if (m->tick_in_bar >= TICKS_PER_BAR) {
        m->tick_in_bar = 0;
        m->bar_in_section++;
        if (m->bar_in_section >= BARS_PER_SECTION) {
            m->bar_in_section = 0;
            start_new_section(m);
        }
    }
}

// ---------------------------------------------------------------
// Render
// ---------------------------------------------------------------

static float render_sample(music_proc_t* m) {
    float out = 0.0f;

    // Bass — saw through lowpass.
    {
        uint32_t const inc = audio_dsp_phase_inc(m->bass_freq);
        float const e = audio_env_tick(&m->bass_env);
        float       s = audio_dsp_saw(m->bass_phase);
        s = audio_biquad_tick(&m->bass_lpf, s);
        m->bass_phase += inc;
        out += s * e * BASS_AMP;
    }

    // Arp — square through lowpass.
    {
        uint32_t const inc = audio_dsp_phase_inc(m->arp_freq);
        float const e = audio_env_tick(&m->arp_env);
        float       s = audio_dsp_square(m->arp_phase);
        s = audio_biquad_tick(&m->arp_lpf, s);
        m->arp_phase += inc;
        out += s * e * ARP_AMP;
    }

    // Pad — three slightly-detuned saws (root + third + fifth)
    // through a lowpass whose cutoff sweeps on a slow LFO.
    {
        uint32_t const inc_a = audio_dsp_phase_inc(m->pad_freq_a * 0.995f);
        uint32_t const inc_b = audio_dsp_phase_inc(m->pad_freq_b);
        uint32_t const inc_c = audio_dsp_phase_inc(m->pad_freq_c * 1.005f);
        float const e   = audio_env_tick(&m->pad_env);
        float const lfo = audio_dsp_sin(m->pad_lfo_phase);
        float       s   = (audio_dsp_saw(m->pad_phase_a)
                        + audio_dsp_saw(m->pad_phase_b)
                        + audio_dsp_saw(m->pad_phase_c)) * (1.0f / 3.0f);
        // Recompute filter every sample is wasteful; lerp the
        // cutoff instead by routing through a fixed filter and
        // tilting amplitude by the LFO.
        s = audio_biquad_tick(&m->pad_lpf, s);
        s *= (0.8f + 0.2f * lfo);
        m->pad_phase_a += inc_a;
        m->pad_phase_b += inc_b;
        m->pad_phase_c += inc_c;
        m->pad_lfo_phase += audio_dsp_phase_inc(0.4f);   // ~0.4 Hz sweep
        out += s * e * PAD_AMP;
    }

    // Kick — sine pitch-drop from 100 Hz to 40 Hz over 80 ms.
    {
        float const e = audio_env_tick(&m->kick_env);
        // Fast pitch decay synthesised from a phase-rate that
        // shrinks with the envelope level — gives the classic
        // "tonk" without modelling it explicitly.
        float const f = 40.0f + e * 60.0f;
        uint32_t const inc = audio_dsp_phase_inc(f);
        float s = audio_dsp_sin(m->kick_phase);
        m->kick_phase += inc;
        out += s * e * KICK_AMP;
    }

    // Snare — bandpassed noise.
    {
        float const e = audio_env_tick(&m->snare_env);
        float n = audio_dsp_noise(&m->snare_noise);
        n = audio_biquad_tick(&m->snare_bpf, n);
        out += n * e * SNARE_AMP;
    }

    // Hi-hat — highpassed noise.
    {
        float const e = audio_env_tick(&m->hat_env);
        float n = audio_dsp_noise(&m->hat_noise);
        n = audio_biquad_tick(&m->hat_hpf, n);
        out += n * e * HAT_AMP;
    }

    return out;
}

static void music_render(music_source_t* self, int16_t* out, size_t frames) {
    music_proc_t* m = (music_proc_t*)self;

    for (size_t i = 0; i < frames; i++) {
        // Tick scheduler.
        if (m->sample_pos >= m->next_tick_at) {
            on_tick(m);
            m->next_tick_at += m->samples_per_16th;
        }

        float const s = render_sample(m);
        int16_t const sample = audio_dsp_to_s16(s);
        out[2 * i + 0] = sample;
        out[2 * i + 1] = sample;

        m->sample_pos += 1.0;
    }
}

// ---------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------

static uint32_t hash32(uint32_t x, char const* tag) {
    // Mix the seed with a stable per-purpose tag so two namespaces
    // ("world", "music") never coincide.
    uint32_t h = x ^ 0xA5A55A5Au;
    for (char const* p = tag; *p; p++) {
        h ^= (uint32_t)(unsigned char)*p;
        h *= 0x01000193u;
    }
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    if (h == 0) h = 1;
    return h;
}

static void music_reseed(music_proc_t* m, uint32_t seed) {
    m->prng = hash32(seed, "music");

    // Pick a tonic in a synthwave-friendly mid-range.
    static int const tonic_choices[] = { 45, 47, 48, 50, 52, 53, 55 };  // A2..G3
    m->root_midi = tonic_choices[prng_range(&m->prng, (uint32_t)(sizeof(tonic_choices) / sizeof(tonic_choices[0])))];

    // Per-run tempo — an integer BPM in [100, 118] so no two seeds
    // feel metronome-identical.
    {
        uint32_t const bpm = MUSIC_BPM_MIN + prng_range(&m->prng, MUSIC_BPM_SPAN);
        m->samples_per_16th =
            (double)AUDIO_SAMPLE_RATE_HZ * 60.0 / ((double)bpm * 4.0);
    }

    // Force a fresh section setup (sets prog, drums, arp, bass,
    // layers). arp_index starts at an impossible value so the
    // first section's no-repeat check can't false-match.
    memset(&m->prog, 0, sizeof(m->prog));
    m->drums       = g_drum_patterns[0];
    m->arp_index   = 0xFFu;
    m->layer_bass  = true;
    m->layer_arp   = true;
    m->layer_pad   = true;
    m->layer_drums = true;
    m->section_index = 0;
    start_new_section(m);
    m->section_index = 0;  // start_new_section bumped it — reset

    // Reset timing.
    m->sample_pos    = 0.0;
    m->next_tick_at  = 0.0;
    m->tick_in_bar   = 0;
    m->bar_in_section= 0;
    m->pad_last_chord_index = -1;

    // Reset all envelopes so a fresh seed doesn't leak the old
    // run's voice tails.
    audio_env_reset(&m->bass_env);
    audio_env_reset(&m->arp_env);
    audio_env_reset(&m->pad_env);
    audio_env_reset(&m->kick_env);
    audio_env_reset(&m->snare_env);
    audio_env_reset(&m->hat_env);
    audio_biquad_reset(&m->bass_lpf);
    audio_biquad_reset(&m->arp_lpf);
    audio_biquad_reset(&m->pad_lpf);
    audio_biquad_reset(&m->snare_bpf);
    audio_biquad_reset(&m->hat_hpf);
}

static void music_on_seed(music_source_t* self, uint32_t seed) {
    music_reseed((music_proc_t*)self, seed);
}

static void music_shutdown(music_source_t* self) {
    heap_caps_free(self);
}

music_source_t* music_procedural_create(uint32_t seed) {
    music_proc_t* m = (music_proc_t*)heap_caps_calloc(1, sizeof(*m), MALLOC_CAP_INTERNAL);
    if (m == NULL) {
        ESP_LOGE(TAG, "alloc failed");
        return NULL;
    }

    m->base.render   = music_render;
    m->base.on_seed  = music_on_seed;
    m->base.shutdown = music_shutdown;

    // Envelope shapes for each voice.
    audio_env_configure(&m->bass_env, 0.005f, 0.18f, 0.5f, 0.06f);
    audio_env_configure(&m->arp_env,  0.002f, 0.08f, 0.0f, 0.04f);
    audio_env_configure(&m->pad_env,  0.40f,  0.30f, 0.7f, 1.20f);
    audio_env_configure(&m->kick_env, 0.001f, 0.08f, 0.0f, 0.001f);
    audio_env_configure(&m->snare_env,0.001f, 0.08f, 0.0f, 0.001f);
    audio_env_configure(&m->hat_env,  0.001f, 0.03f, 0.0f, 0.001f);

    // Filters: bass is dark, arp is bright, pad is warm.
    audio_biquad_lpf(&m->bass_lpf, 600.0f,  0.9f);
    audio_biquad_lpf(&m->arp_lpf,  2400.0f, 0.7f);
    audio_biquad_lpf(&m->pad_lpf,  1200.0f, 0.7f);
    audio_biquad_bpf(&m->snare_bpf, 1800.0f, 0.9f);
    audio_biquad_hpf(&m->hat_hpf,   7000.0f, 0.8f);

    music_reseed(m, seed);

    ESP_LOGI(TAG, "music_procedural_create: seed=%u tonic_midi=%d", (unsigned)seed, m->root_midi);
    return &m->base;
}
