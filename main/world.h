#pragma once

#include <stdbool.h>
#include <stdint.h>

// Phase 3: a stream of cuboid obstacles approaches the camera at the
// ship's forward speed, plus a continuous wall on each side of the
// track. Walls are stored in the same obstacle pool so a single
// future collision pass covers both. Phase 9 will extend the pool
// with pickup kinds (Tris, boost, jump, shield, checkpoint) reusing
// the same per-entry geometry/colour fields.
//
// Pool sized for ~33 wall segments per side (left + right cover the
// 0..100 z range at 3-unit segment length) plus headroom for dynamic
// obstacles and future pickups.
#define WORLD_OBSTACLE_POOL_SIZE 128

// What an obstacle pool entry actually *is* — drives collision
// response and (later) custom rendering. Today only CUBE and WALL
// are populated; the pickup and ramp values are stubbed so the
// collision/render switches already have the slots and can be
// filled in as those phases land. Adding a new kind = one enum
// entry + one case in each switch.
typedef enum {
    OBSTACLE_KIND_CUBE = 0,        // dodge-or-die; head-on fatal, side scrape allowed
    OBSTACLE_KIND_WALL,            // scrape-only (side walls today)
    OBSTACLE_KIND_PICKUP_TRI,      // Phase 6: collected, bumps multiplier counter
    OBSTACLE_KIND_PICKUP_BOOST,    // Phase 5: collected, refills sun + restores speed
    OBSTACLE_KIND_PICKUP_JUMP,     // Phase 9
    OBSTACLE_KIND_PICKUP_SHIELD,   // Phase 9
    OBSTACLE_KIND_RAMP,            // future: contact triggers a jump
} obstacle_kind_t;

typedef struct {
    obstacle_kind_t kind;          // collision + render dispatch
    float    x_world;              // lateral, world units (0 = track centre)
    float    z_world;              // depth, world units (positive = ahead of camera)
    float    half_w;               // half-width  (lateral)
    float    half_d;               // half-depth  (along z)
    float    height;               // height in world units (base sits on y=0 plane)
    uint32_t front_color;          // pax_col_t for the front face
    uint32_t side_color;           // pax_col_t for the visible side face
    uint32_t top_color;            // pax_col_t for the top face (drawn only when camera is above the cube)
    uint32_t outline_color;        // pax_col_t for the wireframe overlay
    bool     active;
} obstacle_t;

typedef struct {
    obstacle_t obstacles[WORLD_OBSTACLE_POOL_SIZE];
    float      next_spawn_z;       // world-z distance until the next dynamic obstacle spawn
    float      right_wall_far_z;   // camera-relative z of the next far-end wall segment to spawn (right side)
    float      left_wall_far_z;    // ditto for the left side
    uint32_t   prng_state;
} world_state_t;

// Initialize the obstacle pool, seed the per-run PRNG, and fill the
// side walls so they're already visible on the very first frame.
// `seed` must be non-zero; pass 1 if you don't have a real seed yet.
void world_init(world_state_t* w, uint32_t seed);

// Advance the world by `dt` seconds at the given forward speed.
// Active obstacles move toward the camera by `speed_z * dt`; those
// that pass the near plane are despawned. Dynamic obstacles spawn at
// the far plane on a randomized world-z interval, so spawn density
// is speed-independent. Wall segments are kept topped up
// deterministically so the side walls stay continuous.
void world_advance(world_state_t* w, float dt, float speed_z);
