// One-shot crash SFX for the ship dying on a head-on collision.
// Filtered noise burst + low pitched-sine thud, ~500 ms total.

#pragma once

#include <stdbool.h>

bool sfx_crash_play(void);
