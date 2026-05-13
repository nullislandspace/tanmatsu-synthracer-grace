#pragma once

#include "obstacle.h"

struct world_state_s;

// Spawn one speed-booster pickup at the far plane, x drawn
// uniformly across the playfield (same range as cube spawns so
// boosters share the lateral distribution).
obstacle_t* booster_spawn(struct world_state_s* w);

// Spawn a booster at an explicit (x, z). Used when the placement
// is significant — e.g. dead-centre in a gateway's gap — rather
// than a uniform random draw.
obstacle_t* booster_spawn_at(struct world_state_s* w, float x, float z);
