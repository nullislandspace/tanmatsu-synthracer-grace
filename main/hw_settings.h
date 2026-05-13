// Per-device hardware settings (display + keyboard + LED brightness,
// speaker + headphone volume) backed by the launcher's NVS namespace
// via the upstream `nvs_settings_*` helpers exported through
// `liball.so`. The launcher persists these — applying them at app
// boot keeps the device experience consistent across launcher/app
// transitions.
//
// Audio jack policy mirrors `volume_howto.md`:
//   - read the launcher's `speaker_volume` when no jack inserted,
//     `headphone_volume` when one is — single ES8156 codec register
//     follows the active output;
//   - speaker amplifier is muted when the jack is in (the codec
//     drives both outputs, but the speaker amp is a separate chip);
//   - VOLUME_UP / VOLUME_DOWN keys step the *currently active*
//     output's NVS value in ±5% increments and re-apply.
//
// Volume / brightness are global codec/peripheral registers; we set
// them once at boot per the howto's "Respect the master volume"
// model, then poke them again only on user input (volume keys or
// jack hot-swap). No per-frame work.

#pragma once

#include <stdbool.h>
#include "esp_err.h"

// Boot-time entry. Reads the persisted speaker/headphone volume,
// display backlight, keyboard backlight, and LED brightness via the
// upstream NVS settings module; applies each via its BSP setter.
// Also reads the initial audio-jack state and picks the right
// volume key + sets amplifier routing.
esp_err_t hw_settings_init(void);

// Handle an audio-jack insert / remove event. Updates the cached
// jack state and re-applies the now-active output's persisted
// volume + amplifier routing. Call from the input event drain on
// `INPUT_EVENT_TYPE_ACTION` with type `BSP_INPUT_ACTION_TYPE_AUDIO_JACK`.
void hw_settings_on_jack_event(bool jack_inserted);

// Step the currently active output's volume by `delta_percent`
// (typically +5 / -5). Clamps to [0, 100], writes back to NVS via
// the upstream helper so the launcher sees the change, and re-
// applies the codec register. Call from the input event drain on
// VOLUME_UP / VOLUME_DOWN.
void hw_settings_step_volume(int delta_percent);
