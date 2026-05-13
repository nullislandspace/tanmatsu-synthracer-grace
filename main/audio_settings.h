// Per-app audio toggles persisted in the `synthracer` NVS namespace.
// Two u8 keys: `audio_music_on` and `audio_sfx_on`. Default for
// both is 1 (enabled) when the keys are missing.
//
// These are *separate* from the system-wide speaker / headphone
// volume which lives in the launcher-shared `"system"` namespace —
// these only mute or unmute this app's music / SFX synthesis
// passes, freeing the CPU when the player has turned them off.

#pragma once

#include "esp_err.h"
#include <stdbool.h>

esp_err_t audio_settings_load(void);

bool audio_settings_music_on(void);
bool audio_settings_sfx_on(void);

void audio_settings_set_music_on(bool on);
void audio_settings_set_sfx_on(bool on);
