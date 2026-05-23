// =====================================================================
//  SynthEngine3D  --  PUBLIC STABLE API  --  procedural music source
//  Part of the semver'd public surface (see se_version.h).
//  NOTE (E2.1, planned): the musical *content* (instruments, scales,
//  chord progressions, tempo/structure) is currently hardcoded as a
//  synthwave personality. A follow-up will lift it into a public
//  se_music_config_t passed at create-time so other games can drive the
//  same generator with their own music. The seed-only signature below
//  will gain a config parameter then.
// =====================================================================
//
// Procedural synthwave music source. Seed-derived: same run seed
// always produces the same musical personality. Uses a separate
// PRNG from the world generator so toggling music on/off never
// perturbs obstacle placement.
//
// Returns a heap-allocated `music_source_t` ready to hand to
// `audio_mixer_set_music()`. The mixer takes ownership; on
// `audio_mixer_set_music(NULL)` (or game-over) the source's
// `shutdown()` callback runs and frees the struct.

#pragma once

#include "se_audio_source.h"
#include <stdint.h>

music_source_t* music_procedural_create(uint32_t seed);
