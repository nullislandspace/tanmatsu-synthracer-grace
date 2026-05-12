#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "magicnumbers.h"

// Phase 3+: a stream of cuboid obstacles approaches the camera at
// the ship's forward speed, plus a continuous wall on each side of
// the track.
//
// World generation is organized into **stages** of fixed world-z
// length (~1 minute of forward travel at base speed). Each stage
// selects obstacle **areas** uniformly at random from the kinds
// whose min_stage <= current stage; an area spawns obstacles
// according to its own rules until its length budget is exhausted,
// then the world picks the next area. When a stage's budget is
// exhausted *after* the current area finishes, a fixed-length rest
// area (no obstacles today, future home of bonus pickups) runs and
// the stage counter ticks over. Determinism comes from a per-stage
// PRNG mixed from the level seed and the stage index, so re-running
// the same seed reproduces every stage identically.
//
// Pool sized for ~33 wall segments per side (left + right cover the
// 0..100 z range at 3-unit segment length), ~5 gateway slab pairs,
// plus headroom for dynamic obstacles and future pickups.
#define WORLD_OBSTACLE_POOL_SIZE 128

// Stage / rest budgets, world-z units. Derived from the tunable
// `GAME_STAGE_SECONDS` / `GAME_REST_SECONDS` × `GAMEPLAY_CRUISE_SPEED`
// in magicnumbers.h. With the current defaults (60s / 10s at cruise
// speed 12 u/s) these come out to 720 / 120 — unchanged numerically
// from the old hardcoded values, but now retunable from one place.
#define WORLD_STAGE_LENGTH_Z     GAME_STAGE_LENGTH_Z
#define WORLD_REST_LENGTH_Z      GAME_REST_LENGTH_Z

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

// Obstacle area types. Each value names a generator kind that knows
// how to populate a span of world-z with obstacles of a particular
// shape and cadence. Adding a new area type = one enum entry + one
// init/tick case in world.c's area dispatch + bumping the min_stage
// table for unlock gating.
typedef enum {
    AREA_TYPE_PIXEL_FIELD = 0,     // small cubes, randomly placed across the track
    AREA_TYPE_GATEWAYS,            // wall slabs spanning the track with one ship-sized opening
    AREA_TYPE_BIG_BLOCKS,          // sparser, larger grey cubes (2× pixel cubes laterally)
    AREA_TYPE_REST,                // empty stretch between stages
} area_type_t;

// Per-area sub-state. The world owns one of these at a time; the
// active type drives how `length_remaining_z` and `next_event_z` are
// interpreted in `area_tick`. The gateway-only fields are unused for
// the other types and zeroed at area-init.
typedef struct {
    area_type_t type;
    float       length_remaining_z;  // world-z left until the area finishes
    float       next_event_z;        // world-z left until the next spawn event inside the area
    int         gates_remaining;     // GATEWAYS only: gates still to spawn
    float       gate_gap_half_w;     // GATEWAYS only: half-width of the opening
    float       gate_pad_z;          // GATEWAYS only: empty z between gates (and lead-in / trailing)
} area_state_t;

typedef struct {
    obstacle_t obstacles[WORLD_OBSTACLE_POOL_SIZE];
    uint8_t    stage;                // 1..N, ticks over after each rest area
    float      stage_z_remaining;    // world-z left in the current stage
    area_state_t area;               // active area's state
    uint32_t   level_seed;           // run-wide seed; preserves stage determinism across stage rollovers
    uint32_t   stage_prng;           // xorshift state mixed from (level_seed, stage); reset each stage
    float      right_wall_far_z;     // camera-relative z of the next far-end wall segment to spawn (right side)
    float      left_wall_far_z;      // ditto for the left side

    // Stage-progress positions (0..GAME_STAGE_LENGTH_Z) at which
    // a booster should spawn during this stage. Set at stage start
    // to N evenly-spaced+jittered values; each slot is overwritten
    // with -1 once its booster has spawned.
    float      booster_due_at_progress[GAME_BOOSTERS_PER_STAGE];
} world_state_t;

// Initialize the obstacle pool, seed the per-run PRNG, fill the side
// walls so they're already visible on the very first frame, and
// start stage 1. `seed` must be non-zero; pass 1 if you don't have a
// real seed yet.
void world_init(world_state_t* w, uint32_t seed);

// Advance the world by `dt` seconds at the given forward speed.
// Active obstacles move toward the camera by `speed_z * dt`; those
// that pass the near plane are despawned. The active area's
// generator is ticked by the same world-z delta and spawns at the
// far plane; when the area finishes, the next one is selected (or a
// rest area, if the stage budget is exhausted). Wall segments are
// kept topped up deterministically so the side walls stay continuous.
void world_advance(world_state_t* w, float dt, float speed_z);
