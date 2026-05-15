#pragma once

#include <stdbool.h>
#include <stdint.h>

struct area_state_s;
struct world_state_s;

// "Simple platform" area (Phase 9.1h). A reserved lead-in gap — where
// the launch ramp will go once 9.1g lands — followed by an elevated
// platform of 10-16 contiguous blocks, each one border-wall segment
// long and wall-grid-aligned. The platform sits just high enough
// that the ship passes under it at ground level and can land on top
// after a jump. Each block carries a Tri on its top face; one is
// swapped for a speed booster if the stage scheduler owes one.
//
// Spawns from stage 2 onward (no upper limit).
void area_simple_platform_init(struct area_state_s* a, uint16_t stage, uint32_t* prng);

// Returns true when the area's length budget is exhausted.
bool area_simple_platform_tick(struct world_state_s* w, struct area_state_s* a, float dz);
