#pragma once
// =====================================================================
//  SynthEngine3D  --  PUBLIC STABLE API  --  device-global hardware settings
// ---------------------------------------------------------------------
//  The launcher-shared hardware settings every Tanmatsu app should
//  respect: speaker / headphone volume (the audio-jack state selects
//  which is live) and the three brightnesses (display backlight, keyboard
//  backlight, LEDs). They are persisted by the launcher in the shared
//  `"system"` NVS namespace via the upstream `nvs_settings_*` helpers;
//  applying them at boot keeps the device experience continuous across
//  launcher <-> app transitions.
//
//  se_run() owns these: it calls se_hw_init() during bootstrap, and its
//  input pump calls se_hw_step_volume() / se_hw_on_jack_event() when it
//  consumes the volume keys / audio-jack action. A game does not call
//  these directly under the framework -- they are public so a non-se_run
//  host (or a settings menu, later) can drive them too.
//
//  Part of the semver'd public surface (see se_version.h).
//
//  Audio-jack policy:
//    - read the persisted speaker volume when no jack is inserted, the
//      headphone volume when one is (one codec register follows the
//      active output);
//    - the speaker amplifier is muted while the jack is inserted;
//    - the volume keys step the *currently active* output's value.
// =====================================================================

#include <stdbool.h>
#include "esp_err.h"

// Boot-time entry (called by se_run during bootstrap). Reads the
// persisted speaker/headphone volume + display/keyboard/LED brightness
// and applies each via its BSP setter; reads the initial audio-jack
// state and picks the active volume + amplifier routing. Per-setter
// failures are logged but non-fatal (graceful degradation).
esp_err_t se_hw_init(void);

// Handle an audio-jack insert / remove event: cache the new state and
// re-apply the now-active output's persisted volume + amplifier routing.
// se_run's input pump calls this on BSP_INPUT_ACTION_TYPE_AUDIO_JACK.
void se_hw_on_jack_event(bool jack_inserted);

// Step the currently active output's volume by `delta_percent` (e.g.
// +5 / -5). Clamps to [0, 100], persists to the shared NVS so the
// launcher sees the change, and re-applies the codec register. se_run's
// input pump calls this on the volume keys.
void se_hw_step_volume(int delta_percent);
