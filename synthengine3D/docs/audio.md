# Audio (`se_audio.h`, `se_audio_source.h`, `se_audio_dsp.h`, `se_music_procedural.h`)

A software mixer that owns the BSP's single I2S channel and sums one **music**
source plus N **SFX voices**. The pipeline is **22050 Hz, signed-16-bit,
stereo (L/R interleaved)** end to end.

## The mixer

`se_run` calls `audio_mixer_init()` at boot (idempotent). The mixer runs on its
own task; the game only hands it sources and pushes mute gates.

```c
esp_err_t audio_mixer_init(void);          // boot (engine does this)
void audio_mixer_shutdown(void);           // mute + stop now (before reboot)

void audio_mixer_set_music(music_source_t* src);     // NULL clears the slot
bool audio_mixer_register_voice(sfx_voice_t* v);     // false if all slots full
void audio_mixer_stop_voice(sfx_voice_t* v);
void audio_mixer_stop_all_voices(void);

void audio_mixer_set_music_enabled(bool on);            // gate the music slot
void audio_mixer_set_group_enabled(uint8_t group, bool on);  // gate an SFX group
```

**Idle power-down:** when the music slot is NULL and every voice is finished,
the mixer feeds silence for a short drain window (~46 ms), then mutes the
speaker amp and disables I2S until a producer pokes it. So "no sound" really
means no power draw — you don't manage that.

**Mute gates are pushed, not pulled.** The mixer never reads your settings; the
host calls `set_music_enabled` / `set_group_enabled` at startup and whenever a
toggle changes. SFX voices carry a `group` index ( `[0, SE_AUDIO_SFX_GROUP_COUNT)` );
the *meaning* of each group is yours (e.g. group 0 general, group 1 a
persistent engine drone with its own mute). All gates default to enabled.

## Source contracts

The mixer only sees two trait structs (in `se_audio_source.h`). You implement
them by **embedding** the struct in your own state so the function pointers and
flags are the entire contract — no registration of types, no allocation rules
beyond "stay alive while in use".

### `music_source_t` — the one music slot

```c
struct music_source_s {
    void (*render)(music_source_t* self, int16_t* stereo_out, size_t frames);
    void (*on_seed)(music_source_t* self, uint32_t seed);   // optional
    void (*shutdown)(music_source_t* self);                 // optional
    // your state follows
};
```

`render` writes exactly `frames` stereo frames (`frames * 2` int16) to a
pre-zeroed buffer; the mixer scales by the music gain (`AUDIO_MUSIC_GAIN`).
`audio_mixer_set_music(NULL)` (or replacing the source, or shutdown) fires
`shutdown()` and forgets the pointer — so the source can free itself there.

### `sfx_voice_t` — short or persistent effects

```c
struct sfx_voice_s {
    void (*render)(sfx_voice_t* self, int16_t* stereo_out, size_t frames);
    void (*shutdown)(sfx_voice_t* self);   // optional
    bool    finished;                       // set true when done -> mixer reaps
    uint8_t group;                          // mute group (app-defined meaning)
    // your state follows
};
```

`render` sums into the pre-zeroed buffer at unity gain (scaled by
`AUDIO_SFX_GAIN`). A one-shot sets `finished = true` when it runs dry; a
persistent voice (a drone) keeps `finished` false and is stopped explicitly via
`audio_mixer_stop_voice()` / `audio_mixer_stop_all_voices()`.

**Callback rules (both kinds):** they run on the mixer task and **must not
block** — no FreeRTOS waits, no NVS, no file I/O, no WARN+ logging. Any state
the audio task reads must already be in place.

### Gain staging

`AUDIO_MUSIC_GAIN` / `AUDIO_SFX_GAIN` (in [`se_config.h`](configuration.md))
set the master music-vs-SFX balance; the mixer hard-clips the int16
accumulator. With several random-phase voices summing by ≈√N, the defaults
leave headroom for music + a few SFX without clipping — retune by ear, then
check the headroom on paper.

## DSP primitives (`se_audio_dsp.h`)

Building blocks for writing a source: oscillators, envelopes, a biquad filter.
Use them inside your `render` callbacks. (See the header for the exact set.)

## Procedural music (`se_music_procedural.h`)

A ready-made `music_source_t`:

```c
music_source_t* music_procedural_create(uint32_t seed);   // hand to the mixer
```

Seed-derived — the same seed always yields the same musical personality, from a
PRNG separate from any world generator so toggling music never perturbs other
seed-driven content.

> **Planned (E2.1):** the musical *content* (instruments, scales, chord
> progressions, tempo/structure) is currently hardcoded as a synthwave
> personality, so today it's reusable only as "a synthwave generator." A
> follow-up lifts that into a public `se_music_config_t` passed at create time,
> adding a config parameter to the signature above (a backwards-compatible
> MINOR change). Until then, drive it with the seed only.
