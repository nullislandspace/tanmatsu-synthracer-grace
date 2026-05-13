#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "magicnumbers.h"
#include "obstacle.h"

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
// Pool sized generously so compound objects (bridges = 3 entries
// each, up to 5 per area; future compounds with more parts) can
// coexist with the always-on side-wall segments (~66 entries) and
// the dynamic area spawns without bumping into the cap.
#define WORLD_OBSTACLE_POOL_SIZE 512

// Stage / rest budgets, world-z units. Derived from the tunable
// `GAME_STAGE_SECONDS` / `GAME_REST_SECONDS` × `GAMEPLAY_CRUISE_SPEED`
// in magicnumbers.h.
#define WORLD_STAGE_LENGTH_Z     GAME_STAGE_LENGTH_Z
#define WORLD_REST_LENGTH_Z      GAME_REST_LENGTH_Z

// World-z thresholds shared across object spawners and the despawn
// pass. Far spawn: where new obstacles enter the world. Near despawn:
// once an obstacle's *back edge* (z + half_d) crosses this it's
// removed from the pool.
#define WORLD_Z_FAR_SPAWN        100.0f
#define WORLD_Z_NEAR_DESPAWN     0.6f

// Track geometry. Playfield is `2 * TRACK_HALF_WIDTH` wide, centred
// on x=0. Used by the random x-distribution in object spawners and
// by the side-wall placement.
#define TRACK_HALF_WIDTH         5.0f

// `obstacle_t` and `obstacle_kind_t` live in obstacle.h (transitively
// included above). The pool itself is owned by world_state_t below.

// Obstacle area types. Each value names a generator kind that knows
// how to populate a span of world-z with obstacles of a particular
// shape and cadence. Adding a new area type = one enum entry + one
// dispatch case in world.c's area picker + bumping its min_stage for
// unlock gating.
typedef enum {
    AREA_TYPE_PIXEL_FIELD = 0,     // small cubes, randomly placed across the track
    AREA_TYPE_GATEWAYS,            // wall slabs spanning the track with one ship-sized opening
    AREA_TYPE_BIG_BLOCKS,          // sparser, larger grey cubes (2× pixel cubes laterally)
    AREA_TYPE_BRIDGES,             // concrete archways spanning the track — visual + shadow only
    AREA_TYPE_REST,                // empty stretch between stages
} area_type_t;

// Per-area sub-state. The world owns one of these at a time; the
// active type drives how `length_remaining_z` and `next_event_z` are
// interpreted by each area's tick function. The gateway-only fields
// are unused for the other types and zeroed at area-init.
typedef struct area_state_s {
    area_type_t type;
    float       length_remaining_z;  // world-z left until the area finishes
    float       next_event_z;        // world-z left until the next spawn event inside the area
    int         gates_remaining;     // GATEWAYS only: gates still to spawn
    float       gate_gap_half_w;     // GATEWAYS only: half-width of the opening
    float       gate_pad_z;          // GATEWAYS only: empty z between gates (and lead-in / trailing)
    int         boosters_owed;       // count of boosters the top-level scheduler has flagged but not placed
} area_state_t;

typedef struct world_state_s {
    obstacle_t obstacles[WORLD_OBSTACLE_POOL_SIZE];
    uint16_t   stage;                // 1..N, ticks over after each rest area (16 bits → effectively unlimited)
    float      stage_z_remaining;    // world-z left in the current stage
    area_state_t area;               // active area's state
    uint32_t   level_seed;           // run-wide seed; preserves stage determinism across stage rollovers
    uint32_t   stage_prng;           // xorshift state mixed from (level_seed, stage); reset each stage
    float      right_wall_far_z;     // camera-relative z of the next far-end wall segment to spawn (right side)
    float      left_wall_far_z;      // ditto for the left side

    // Stage-progress positions (0..GAME_STAGE_LENGTH_Z) at which a
    // booster should be flagged-due during this stage. Set at stage
    // start to N evenly-spaced+jittered values; each slot is
    // overwritten with -1 once its scheduled point fires.
    float      booster_due_at_progress[GAME_BOOSTERS_PER_STAGE];

    // Debug-only override: when ≥ 0, the next `start_next_area` call
    // uses this area type instead of the picker (bypassing all
    // min-stage gating). Cleared as soon as it's consumed.
    // Set via `world_force_next_area`, typically wired to a debug
    // key in main.c. -1 = no override.
    int        forced_next_area_type;
} world_state_t;

// Initialize the obstacle pool, seed the per-run PRNG, fill the side
// walls so they're already visible on the very first frame, and
// start stage 1. `seed` must be non-zero; pass 1 if you don't have a
// real seed yet.
void world_init(world_state_t* w, uint32_t seed);

// Advance the world by `dt` seconds at the given forward speed. The
// pool sweep advances every active obstacle's z, fires each one's
// optional physics callback (with `cam_x` so objects can react to
// lateral ship position), and despawns anything that's drifted past
// the near plane. The active area's tick generates new spawns at
// the far plane; when the area finishes, world picks the next one
// (or inserts a rest area at stage end). Side walls are kept topped
// up so the track edges stay continuous.
void world_advance(world_state_t* w, float dt, float speed_z, float cam_x);

// Debug: force the next `start_next_area` call to use `t`, bypassing
// the area picker (and any min-stage gating it might gain). Also
// cuts the current area's length budget to zero so the override
// takes effect on the very next world-advance pass instead of
// waiting for the current area to finish naturally. If the stage
// budget is also exhausted by then, the rest area inserts first and
// the override applies after the stage rollover.
void world_force_next_area(world_state_t* w, area_type_t t);

// --- Shared PRNG / staging helpers ---------------------------------
// Used by the area generators in main/areas/*.c. xorshift state is
// per-stage so content is reproducible from (level_seed, stage).

uint32_t world_xorshift32(uint32_t* s);
float    world_frand(uint32_t* s);                       // uniform [0, 1)
float    world_lerp_by_stage(uint16_t stage, float at_one, float at_ten);
float    world_stage_interval_scale(uint16_t stage);
