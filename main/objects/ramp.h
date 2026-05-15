#pragma once

#include "obstacle.h"

struct world_state_s;

// Ramp (Phase 9.1g) — a visible dull-yellow wedge. Driving into a
// ramp is not a collision: the ship rides its sloped top and leaves
// the far lip already climbing, so it launches into the air. The
// launch is emergent — the wedge rises from world-y 0 at its near
// face to world-y `rise` at its far face, and game_step's support
// pass turns the slope (`rise / (2*half_d)`) into a climb rate of
// `ship_speed_z * slope`; that velocity is what the ship keeps when
// the ramp ends. Steeper or faster ⇒ more air, for free.
//
// Returns the spawned obstacle, or NULL if the pool is full.
obstacle_t* ramp_spawn_at(struct world_state_s* w, float x, float z,
                          float half_w, float half_d, float rise);
