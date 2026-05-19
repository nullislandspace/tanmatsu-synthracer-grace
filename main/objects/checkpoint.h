#pragma once

#include "obstacle.h"

struct world_state_s;

// Checkpoint pickup (Phase 9.3) — a slowly-rotating black-and-white
// icosahedron, a soccer-ball reskin of the speed booster. Collecting
// it snapshots the whole run state; on a later head-on crash the run
// rewinds to that snapshot. Spawned in place of a Tri when the stage
// scheduler owes the area a checkpoint (see world_place_pickup).
//
// Returns the spawned obstacle, or NULL if the pool is full.
obstacle_t* checkpoint_spawn_at(struct world_state_s* w, float x, float z);

// Spawn a checkpoint at the far plane at a random wall-cleared x.
obstacle_t* checkpoint_spawn(struct world_state_s* w);
