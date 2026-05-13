#pragma once

#include <stdbool.h>
#include <stdint.h>

struct area_state_s;
struct world_state_s;

// Dynamic passage — a flipping-cube area with two visually-mirrored
// subtypes. The init helper rolls one extra coin from the stage PRNG
// to pick non-mirrored vs mirrored, so this area type is one slot
// in the picker but produces two distinct layouts in roughly equal
// proportion.
//
// Layout (non-mirrored):
//   - 4..7 left-rolling flipping cubes spawn along the right wall,
//     one cube-depth apart (gap = depth, centre-to-centre = 2 × depth).
//   - The rest of the playfield (everywhere x is not occupied by
//     the flipping-cube column or its just-left-of-it bait corridor)
//     is densely sprinkled with pixel-field cubes.
//   - A narrow corridor just to the left of the flipping cubes is
//     deliberately left clear — this is the *apparent* safe path,
//     bait for the player.
//   - As the cubes near the camera they roll 90° to the left, ending
//     in that bait corridor (blocking it). The actual safe lane is
//     where the cubes used to be — pressed against the right wall.
//
// Layout (mirrored): left/right swapped; right-rolling cubes against
// the left wall, bait corridor to their right, safe lane along the
// left wall after the roll.
//
// Boosters scheduled during this area land in the wall-adjacent
// safe lane, in the z-gap between successive flipping cubes. This
// rewards a player who recognises the layout and commits to the
// wall lane rather than threading the bait.
void area_dynamic_passage_init(struct area_state_s* a, uint16_t stage, uint32_t* prng);

bool area_dynamic_passage_tick(struct world_state_s* w, struct area_state_s* a, float dz);
