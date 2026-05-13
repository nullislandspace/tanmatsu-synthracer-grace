// Procedural synthwave music source. Seed-derived: same run seed
// always produces the same musical personality. Uses a separate
// PRNG from the world generator (split off in main.c) so toggling
// music on/off never perturbs obstacle placement.
//
// Returns a heap-allocated `music_source_t` ready to hand to
// `audio_mixer_set_music()`. The mixer takes ownership; on
// `audio_mixer_set_music(NULL)` (or game-over) the source's
// `shutdown()` callback runs and frees the struct.

#pragma once

#include "audio_source.h"
#include <stdint.h>

music_source_t* music_procedural_create(uint32_t seed);
