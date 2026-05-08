#pragma once

#include <stdbool.h>
#include <stdint.h>

// Phase 3: a single stream of cuboid obstacles approaches the camera at
// the ship's forward speed. Phase 9 will extend the pool with pickup
// kinds (Tris, boost, jump, shield, checkpoint).

#define WORLD_OBSTACLE_POOL_SIZE 64

typedef struct {
    float x_world;   // lateral, world units (0 = track centre)
    float z_world;   // depth, world units (positive = ahead of camera)
    float half_w;    // half-width in world units
    float height;    // height in world units (base sits on y=0 plane)
    bool  active;
} obstacle_t;

typedef struct {
    obstacle_t obstacles[WORLD_OBSTACLE_POOL_SIZE];
    float      next_spawn_z;   // remaining world-z distance until the next spawn
    uint32_t   prng_state;
} world_state_t;

// Initialize the obstacle pool and seed the per-run PRNG. `seed` must
// be non-zero; pass 1 if you don't have a real seed yet.
void world_init(world_state_t* w, uint32_t seed);

// Advance the world by `dt` seconds at the given forward speed.
// Obstacles move toward the camera by `speed_z * dt`; those that pass
// the near plane are despawned. New obstacles spawn at the far plane on
// a randomized world-z interval, so spawn density is speed-independent.
void world_advance(world_state_t* w, float dt, float speed_z);
