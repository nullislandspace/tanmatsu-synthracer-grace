// One-shot "melodic gong" SFX — a struck metallic tone: a low
// fundamental plus a stack of harmonic and slightly-inharmonic
// partials with a long (~2 s) decay, the higher partials fading
// faster. Used for the Phase 9.3 checkpoint pickup.

#pragma once

#include <stdbool.h>

// Play one gong strike. Multiple overlapping plays are supported up
// to the mixer's free voice slots. Returns true on success.
bool sfx_gong_play(void);
