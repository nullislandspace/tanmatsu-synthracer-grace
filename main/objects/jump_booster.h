#pragma once

#include "obstacle.h"

struct world_state_s;

// Jump-booster pickup (Phase 9.1f) — a red rotating octahedron.
// Collected in game_collide (OBSTACLE_KIND_PICKUP_JUMP): grants one
// jump charge (game_state.jump_charges, capped at
// GAME_JUMP_CHARGE_MAX) and counts toward the run's pickups_jump.
//
// jump_booster_spawn places one at the far plane with a random x;
// jump_booster_spawn_at places one at an explicit (x, z). Both set
// the octahedron draw callback. Return the obstacle, or NULL if the
// pool is full (drop silently — jump boosters are bonus pickups).
obstacle_t* jump_booster_spawn(struct world_state_s* w);
obstacle_t* jump_booster_spawn_at(struct world_state_s* w, float x, float z);
