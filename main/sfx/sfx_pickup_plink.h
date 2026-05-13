// One-shot ascending-pentatonic "plink" for Tri pickup. Plays a
// single bell-like sine pulse whose pitch depends on the Tri's
// slot in the current multiplier cycle — pickups 1..5 within
// one cycle play C5, D5, E5, G5, A5 respectively (C-major
// pentatonic, ascending), so the 5 picks audibly fill the HUD
// multiplier-progress row and resolve on the 5th note as the
// multiplier ticks up.
//
// The caller passes the slot index 0..4. The mapping from
// `game_state.pickups_tri` to slot is `(pickups_tri - 1) % 5`
// (after the increment for the current pickup), which yields
// 0,1,2,3,4 across the five pickups in a cycle and wraps back
// to 0 for the start of the next cycle.

#pragma once

#include <stdbool.h>

// Play one plink note. `slot_index` is clamped to [0, 4]; out-of-
// range values fall back to slot 0 (lowest pitch). Multiple
// overlapping plinks are supported up to the mixer's free SFX
// voice slots — they don't share state.
bool sfx_pickup_plink_play(int slot_index);
