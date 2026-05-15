#pragma once

#include <stdbool.h>
#include <stdint.h>

struct area_state_s;
struct world_state_s;

// Rest area = empty stretch between stages. No tick-driven spawns;
// boosters owed at rest entry are dumped all at once by the
// world-level area-transition handler (see world.c).
//
// `length_z` is the rest stretch's world-z budget: between-stage
// rests pass WORLD_REST_LENGTH_Z, while the short pre-stage-1 intro
// lead-in passes a single screen depth (WORLD_Z_FAR_SPAWN).
void area_rest_init(struct area_state_s* a, float length_z);

// Returns true when the rest area's length budget is exhausted.
// Doesn't itself spawn anything.
bool area_rest_tick(struct world_state_s* w, struct area_state_s* a, float dz);
