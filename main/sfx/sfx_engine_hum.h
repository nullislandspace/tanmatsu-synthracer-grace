// Persistent engine-hum SFX voice. Active during STATE_PLAYING
// (and STATE_PAUSED — pause is "still in the run"). Pitch tracks
// `ship_speed_z` via `sfx_engine_hum_set_pitch()`; the voice
// reads the latched value at the start of each mixer chunk so
// it's sample-accurate but doesn't smear during fast speed
// changes.

#pragma once

#include <stdbool.h>

// Start the engine hum. Idempotent — calling while already
// running is a no-op. Returns true on success (registered with
// the mixer), false if the voice could not be allocated.
bool sfx_engine_hum_start(void);

// Stop and tear down the voice. Idempotent.
void sfx_engine_hum_stop(void);

// Update the pitch driver. `speed_normalised` is the ship's
// forward speed mapped into [0.0, 1.0] (game caller converts
// from world units). 0 = idle whisper, 1 = full chase.
void sfx_engine_hum_set_pitch(float speed_normalised);
