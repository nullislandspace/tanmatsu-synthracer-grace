// Interfaces shared between the audio mixer and its plug-in sources.
//
// The mixer (`audio_mixer.c`) only sees two trait-style structs:
//
//   - `music_source_t` — one slot, lives for the duration of a run
//     (or until replaced). The procedural synthwave generator
//     implements this today; future modplayer / MP3 / MIDI sources
//     will implement it the same way without the mixer needing to
//     change.
//
//   - `sfx_voice_t` — short-lived (one-shot) or long-lived
//     (persistent, e.g. engine hum). The owning effect module
//     (`sfx/sfx_*.c`) embeds one of these in its per-instance
//     state struct so the embedded function pointers and the
//     `finished` flag are the mixer's only API contract.
//
// Both render callbacks produce **22050 Hz, signed-16-bit, stereo
// (L/R interleaved)** samples — the one format used end-to-end on
// the audio path. `frames` is the number of stereo *frames* to
// render (so `frames * 2` int16 samples written to `out`). The
// mixer guarantees the output buffer is zeroed before the call.
//
// All callbacks run on the mixer task. They must not block (no
// FreeRTOS waits, no NVS, no file I/O, no logging at WARN+).
// State the audio task reads must already be there.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AUDIO_SAMPLE_RATE_HZ 22050u

// ---------------------------------------------------------------
// Music source
// ---------------------------------------------------------------

typedef struct music_source_s music_source_t;

struct music_source_s {
    // Write exactly `frames` stereo frames to `stereo_out`. Output
    // buffer is pre-zeroed; the mixer scales the result by the
    // music gain (≈ 30%) before mixing. Must never block.
    void (*render)(music_source_t* self, int16_t* stereo_out, size_t frames);

    // Optional: reseed the source mid-run (e.g. after STATE_GAME_OVER
    // → STATE_PLAYING with a fresh seed). May be NULL.
    void (*on_seed)(music_source_t* self, uint32_t seed);

    // Optional: free any internal allocations. Called from
    // `audio_mixer_set_music(NULL)` or `audio_mixer_shutdown()`.
    // May be NULL.
    void (*shutdown)(music_source_t* self);

    // Backend-specific state follows in the embedding struct.
};

// ---------------------------------------------------------------
// SFX voice
// ---------------------------------------------------------------

typedef struct sfx_voice_s sfx_voice_t;

// Which Audio-settings toggle gates a given voice. One-shot SFX
// (ding, crash, scrape, cube-bump, plink) share the
// `audio_settings_sfx_on` flag; the engine hum has its own
// `audio_settings_hum_on` flag so players who like the SFX cues
// but find the constant low drone fatiguing can mute just the
// hum without losing the rest.
typedef enum {
    SFX_VOICE_TAG_ONESHOT = 0,   // gated by audio_settings_sfx_on
    SFX_VOICE_TAG_HUM,           // gated by audio_settings_hum_on
} sfx_voice_tag_t;

struct sfx_voice_s {
    // Render `frames` stereo frames to `stereo_out`. Output buffer
    // is pre-zeroed; the mixer sums the result at unity gain. Set
    // `self->finished = true` when the voice has nothing more to
    // produce — the mixer reaps it after the current chunk.
    void (*render)(sfx_voice_t* self, int16_t* stereo_out, size_t frames);

    // Optional: free any internal allocations. Called by the mixer
    // when reaping the voice. May be NULL.
    void (*shutdown)(sfx_voice_t* self);

    // Mixer sets this to false when the voice is registered; the
    // voice sets it to true when done. For persistent voices the
    // owning module either keeps a pointer and toggles the flag
    // itself, or calls `audio_mixer_stop_voice(self)` (which sets
    // the flag for it).
    bool finished;

    // Which Audio-settings toggle gates this voice. Defaults to
    // SFX_VOICE_TAG_ONESHOT — voices that don't set this
    // explicitly are treated as ordinary one-shots and follow the
    // main SFX gate.
    sfx_voice_tag_t tag;

    // Backend-specific state follows in the embedding struct.
};
