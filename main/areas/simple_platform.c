#include "areas/simple_platform.h"

#include <math.h>

#include "objects/booster.h"
#include "objects/ramp.h"
#include "objects/tri.h"
#include "objects/wall.h"   // WALL_SEGMENT_LEN / WALL_SEGMENT_HALF_D
#include "world.h"

// Block count range — 10 to 16 contiguous blocks form the platform.
#define SP_BLOCK_MIN          10
#define SP_BLOCK_MAX          16

// Block geometry. Length (z) is one border-wall segment, wall-grid
// aligned. Width is a comfortable landing strip. y_base sits just
// above the ship's grounded collision top (SHIP_BASE_Y +
// SHIP_COLLISION_HEIGHT = 0.52) so the ship passes cleanly under;
// the top face sits at y_base + height = 2.0 — above a plain jump's
// reach (~1.78), so the launch ramp is the intended way up.
#define SP_BLOCK_HALF_W       1.5f
#define SP_BLOCK_HALF_D       WALL_SEGMENT_HALF_D
#define SP_BLOCK_Y_BASE       1.5f
#define SP_BLOCK_HEIGHT       0.5f

// Lead-in: clear run before the first block. The launch ramp sits
// at the head of it; the rest is the gap the ship arcs across after
// the ramp launches it onto the platform.
#define SP_LEAD_Z             50.0f
// Trailing clear run after the last block.
#define SP_TRAIL_Z            30.0f

// The launch ramp at the head of the lead-in (Phase 9.1g). Its
// width matches the platform so a ship lined up for one is lined up
// for the other; rise + depth set the launch arc onto the platform.
#define SP_RAMP_HALF_D        5.0f    // depth = 10 u
#define SP_RAMP_RISE          1.2f

// Platform palette — slate blue, distinct from the magenta walls,
// amber gates, grey bridges and green rest posts. Cyan wireframe
// matches the rest of the world's hard-surface objects.
#define SP_FRONT_COLOR        0xFF5A6E8Cu
#define SP_SIDE_COLOR         0xFF36445Au
#define SP_TOP_COLOR          0xFF7E92B4u
#define SP_OUTLINE_COLOR      0xFF31FBFBu

// Snap a z down to the nearest wall-segment centre at or below it,
// so the platform blocks land on the wall grid (same alignment
// trick the bridges and rest areas use).
static float snap_to_wall_segment(float target) {
    int const n = (int)floorf((target - WALL_SEGMENT_HALF_D) / WALL_SEGMENT_LEN);
    return (float)n * WALL_SEGMENT_LEN + WALL_SEGMENT_HALF_D;
}

void area_simple_platform_init(area_state_t* a, uint16_t stage, uint32_t* prng) {
    (void)stage;  // block count + platform x are stage-agnostic

    int n = SP_BLOCK_MIN
          + (int)(world_frand(prng) * (float)(SP_BLOCK_MAX - SP_BLOCK_MIN + 1));
    if (n > SP_BLOCK_MAX) n = SP_BLOCK_MAX;

    // Platform x — random, inset so the full block width fits inside
    // the playfield. Stashed in gate_hole_x (a generic per-area
    // float slot) and shared by every block + pickup in the area.
    float const x_extent = TRACK_HALF_WIDTH - SP_BLOCK_HALF_W;
    a->gate_hole_x = (world_frand(prng) * 2.0f - 1.0f) * x_extent;

    a->type               = AREA_TYPE_SIMPLE_PLATFORM;
    a->gates_remaining    = n;     // reused as the block counter
    a->passage_mirror     = 0;     // reused: 0 = launch ramp not yet placed
    // Layout: [lead-in][block][block]...[block][trailing]. Blocks
    // are contiguous — one wall segment apart — so they read as a
    // single continuous platform.
    a->length_remaining_z = SP_LEAD_Z + (float)n * WALL_SEGMENT_LEN + SP_TRAIL_Z;
    a->next_event_z       = SP_LEAD_Z;
    // boosters_owed deliberately left untouched so a stale owed
    // booster from an earlier area carries into this one.
}

bool area_simple_platform_tick(world_state_t* w, area_state_t* a, float dz) {
    a->length_remaining_z -= dz;
    a->next_event_z       -= dz;

    // The launch ramp — spawned once at the head of the area, on the
    // platform's lateral line. The ship rides it up and arcs onto
    // the platform. `passage_mirror` is the "ramp placed" flag.
    if (a->passage_mirror == 0) {
        ramp_spawn_at(w, a->gate_hole_x, WORLD_Z_FAR_SPAWN,
                      SP_BLOCK_HALF_W, SP_RAMP_HALF_D, SP_RAMP_RISE);
        a->passage_mirror = 1;
    }

    float const spawn_z   = snap_to_wall_segment(WORLD_Z_FAR_SPAWN);
    float const x         = a->gate_hole_x;
    float const block_top = SP_BLOCK_Y_BASE + SP_BLOCK_HEIGHT;

    while (a->next_event_z <= 0.0f && a->gates_remaining > 0) {
        // One elevated platform block. y_base lifts it clear of the
        // ground; the default cube renderer honours y_base.
        obstacle_t* const block = obstacle_spawn(
            w, OBSTACLE_KIND_CUBE,
            x, spawn_z,
            SP_BLOCK_HALF_W, SP_BLOCK_HALF_D, SP_BLOCK_HEIGHT,
            SP_FRONT_COLOR, SP_SIDE_COLOR, SP_TOP_COLOR, SP_OUTLINE_COLOR);
        if (block) block->y_base = SP_BLOCK_Y_BASE;

        // A pickup resting on the block's top face — a booster if
        // the stage scheduler owes one (the first such block claims
        // it), otherwise a Tri.
        if (a->boosters_owed > 0) {
            obstacle_t* const b = booster_spawn_at(w, x, spawn_z);
            if (b) b->y_base = block_top;
            a->boosters_owed--;
        } else {
            obstacle_t* const t = tri_spawn_at(w, x, spawn_z);
            if (t) t->y_base = block_top;
        }

        a->gates_remaining--;
        a->next_event_z += WALL_SEGMENT_LEN;
    }

    return a->length_remaining_z <= 0.0f;
}
