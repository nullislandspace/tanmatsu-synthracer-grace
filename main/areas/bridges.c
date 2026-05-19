#include "areas/bridges.h"

#include <math.h>

#include "magicnumbers.h"
#include "objects/booster.h"
#include "objects/bridge.h"
#include "objects/tri.h"
#include "objects/wall.h"   // for WALL_SEGMENT_LEN / WALL_SEGMENT_HALF_D
#include "world.h"

#define BRIDGES_COUNT_MIN  1
#define BRIDGES_COUNT_MAX  5

// Snap a target spawn z down to the nearest wall-segment centre at
// or below `target`. Wall segment centres are at
// `n * WALL_SEGMENT_LEN + WALL_SEGMENT_HALF_D` for integer n ≥ 0;
// bridges align to those so they sit cleanly on top of the wall
// grid (and on the corresponding floor stripe).
static float snap_to_wall_segment(float target) {
    int   const n   = (int)floorf((target - WALL_SEGMENT_HALF_D) / WALL_SEGMENT_LEN);
    float const z   = (float)n * WALL_SEGMENT_LEN + WALL_SEGMENT_HALF_D;
    return z;
}

void area_bridges_init(area_state_t* a, uint16_t stage, uint32_t* prng) {
    (void)stage;  // stage-agnostic per spec

    int n = BRIDGES_COUNT_MIN
          + (int)(world_frand(prng) * (float)(BRIDGES_COUNT_MAX - BRIDGES_COUNT_MIN + 1));
    if (n > BRIDGES_COUNT_MAX) n = BRIDGES_COUNT_MAX;

    // Layout: [lead-gap][bridge][gap][bridge][gap]...[bridge][trail-gap].
    // Gap = 2 × BRIDGE_DEPTH (= 6 u, = 2 wall segments) so consecutive
    // bridge centres are 3 × BRIDGE_DEPTH = 9 u apart, which is also
    // 3 wall segments — alignment preserved. The wider gap keeps the
    // shadow strips on the floor visually separated even when the
    // sun is mid-way down and the projected shadow extends a few
    // units toward the camera.
    float const gap   = BRIDGE_DEPTH * 2.0f;
    float const thick = BRIDGE_DEPTH;
    a->type               = AREA_TYPE_BRIDGES;
    a->gates_remaining    = n;        // reuse this slot as the bridge counter
    a->gate_pad_z         = gap;
    a->gate_gap_half_w    = 0.0f;     // unused for this area type
    a->length_remaining_z = (float)(n + 1) * gap + (float)n * thick;
    a->next_event_z       = gap;      // one gap of lead-in before the first bridge
    // boosters_owed deliberately left untouched so unfulfilled
    // boosters carry over from the previous area into this one.

    // Phase 6 Tri line. Pick start- and end-x independently and
    // uniformly within the playfield (inset by a Tri half-width so
    // endpoints can't clip the border wall). Each bridge gets a Tri
    // at the lerp position along this line; the line can be at any
    // angle including degenerate (start ≈ end) — that's a feature,
    // not a bug, it gives the player occasional "stacked column of
    // Tris" runs that are easy mode visually.
    float const tri_x_extent = TRACK_HALF_WIDTH - GAME_TRI_HALF_W;
    a->tri_line_x0 = (world_frand(prng) * 2.0f - 1.0f) * tri_x_extent;
    a->tri_line_x1 = (world_frand(prng) * 2.0f - 1.0f) * tri_x_extent;
    a->passage_mirror = n;  // total bridge count (see world.h overload doc)
}

bool area_bridges_tick(world_state_t* w, area_state_t* a, float dz) {
    a->length_remaining_z -= dz;
    a->next_event_z       -= dz;

    // Each event spawns one bridge plus, if owed, one floor booster
    // anywhere across the track at the far plane. The bridge itself
    // is anchored to the nearest wall-segment centre at or below
    // WORLD_Z_FAR_SPAWN so it lands on the wall grid every time.
    float const spawn_z = snap_to_wall_segment(WORLD_Z_FAR_SPAWN);
    float const thick   = BRIDGE_DEPTH;

    while (a->next_event_z <= 0.0f && a->gates_remaining > 0) {
        bridge_spawn(w, spawn_z);

        // Phase 6: one Tri per bridge, placed at the lerp position
        // along the area's pre-picked line. Bridge index is
        // (n_total - gates_remaining) BEFORE we decrement
        // gates_remaining below — index 0 for the first spawn,
        // n_total-1 for the last. With n_total == 1 we use t = 0
        // (the line's start endpoint) to avoid a divide-by-zero.
        int   const n_total      = a->passage_mirror;
        int   const bridge_index = n_total - a->gates_remaining;
        float       t            = 0.0f;
        if (n_total > 1) {
            t = (float)bridge_index / (float)(n_total - 1);
        }
        float const tri_x = a->tri_line_x0 + (a->tri_line_x1 - a->tri_line_x0) * t;
        world_place_pickup(w, a, tri_x, spawn_z);

        a->gates_remaining--;
        a->next_event_z += a->gate_pad_z + thick;

        if (a->boosters_owed > 0) {
            booster_spawn(w);
            a->boosters_owed--;
        }
    }

    return a->length_remaining_z <= 0.0f;
}
