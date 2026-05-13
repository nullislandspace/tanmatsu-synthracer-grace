// One-shot "ding" SFX for speed-booster pickup. Two short sine
// tones a major-third apart with a fast decay envelope — same
// shape as an electric doorbell. ~250 ms total.

#pragma once

#include <stdbool.h>

// Play one instance. Multiple overlapping plays are supported up
// to the mixer's free voice slots. Returns true on success.
bool sfx_pickup_ding_play(void);
