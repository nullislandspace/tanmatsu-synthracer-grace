#pragma once

#include "obstacle.h"

struct world_state_s;

// Shield pickup (Phase 9.2) — a slowly-rotating violet hexagonal
// plate. Collecting it grants a shield charge; a charged shield
// absorbs one head-on crash instead of ending the run. Spawned in
// place of a Tri when the stage scheduler owes the area a shield
// (see world_place_pickup).
//
// Returns the spawned obstacle, or NULL if the pool is full.
obstacle_t* shield_spawn_at(struct world_state_s* w, float x, float z);

// Spawn a shield at the far plane at a random wall-cleared x — the
// convenience form used for area-entry / lead-in placement (mirrors
// jump_booster_spawn).
obstacle_t* shield_spawn(struct world_state_s* w);
