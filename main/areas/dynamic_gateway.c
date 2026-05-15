#include "areas/dynamic_gateway.h"

#include "objects/booster.h"
#include "objects/cube.h"
#include "objects/flipping_cube.h"
#include "objects/tri.h"
#include "world.h"

#define DYN_GATE_COUNT_MIN   3
#define DYN_GATE_COUNT_MAX   5

// Inter-wall padding. The standard gateways area uses 50 u because
// the hole can shift from one extreme to the other between walls
// and the player has to physically traverse the width of the track
// to re-align. Here the hole is fixed for the whole area run, so
// the player holds position rather than steering — the pad only
// needs to give the cubes room to cascade through their rolls
// without visually overlapping each other. 10 u (= 0.5 s at cruise)
// keeps the walls coming at a punchy rhythm and produces a nice
// staggered chain of rolling cubes behind them.
#define DYN_GATE_PAD_Z       10.0f

// Hole geometry. Half-width = FLIPPING_CUBE_HALF_W (so the cube
// behind the wall exactly fills the hole until it rolls).
#define DYN_GATE_HOLE_HALF_W FLIPPING_CUBE_HALF_W

// Z offset of the flipping cube behind the gate plane. We want the
// cube's near face flush with the wall's back face: cube_z =
// wall_z + wall_half_d + cube_half_d. With CUBE_GATE_HALF_D = 0.4
// and FLIPPING_CUBE_HALF_D = 0.8 that's a 1.2 u offset. Painter's
// sort renders the cube first (further z), then the wall slabs on
// top, so the wall correctly occludes the cube — including the
// rolled cube's tail end after it's landed against a slab.
#define DYN_GATE_CUBE_Z_OFFSET (CUBE_GATE_HALF_D + FLIPPING_CUBE_HALF_D)

static void spawn_dyn_gate(world_state_t* w, float hole_x) {
    float const half_gap     = DYN_GATE_HOLE_HALF_W;
    float const left_inner   = hole_x - half_gap;
    float const left_outer   = -TRACK_HALF_WIDTH;
    float const right_inner  = hole_x + half_gap;
    float const right_outer  =  TRACK_HALF_WIDTH;
    float const left_half_w  = (left_inner  - left_outer) * 0.5f;
    float const left_centre  = (left_inner  + left_outer) * 0.5f;
    float const right_half_w = (right_outer - right_inner) * 0.5f;
    float const right_centre = (right_outer + right_inner) * 0.5f;
    cube_spawn_gate_slab(w, left_centre,  WORLD_Z_FAR_SPAWN, left_half_w);
    cube_spawn_gate_slab(w, right_centre, WORLD_Z_FAR_SPAWN, right_half_w);

    // Direction picks the side the cube rolls toward — always toward
    // the playfield centre. `hole_x >= 0` snaps to left-roll, so the
    // exactly-centre case has no deadband (left-roll wins ties).
    int const dir = (hole_x >= 0.0f) ? -1 : +1;
    flipping_cube_spawn(w, hole_x,
                        WORLD_Z_FAR_SPAWN + DYN_GATE_CUBE_Z_OFFSET,
                        dir);
}

void area_dynamic_gateway_init(area_state_t* a, uint16_t stage, uint32_t* prng) {
    (void)stage;  // count + hole-x both stage-agnostic today

    int n = DYN_GATE_COUNT_MIN
          + (int)(world_frand(prng) * (float)(DYN_GATE_COUNT_MAX - DYN_GATE_COUNT_MIN + 1));
    if (n > DYN_GATE_COUNT_MAX) n = DYN_GATE_COUNT_MAX;

    // Hole x — picked once for the whole area, drawn so the hole
    // (half-width = DYN_GATE_HOLE_HALF_W) fits inside the playfield
    // by definition. With the standard track ±5 and cube half-width
    // 0.8 that's a [-4.2, +4.2] range; both wall slabs stay non-
    // degenerate at the extremes (slab thickness >= 0.8).
    float const x_extent = TRACK_HALF_WIDTH - DYN_GATE_HOLE_HALF_W;
    a->gate_hole_x = (world_frand(prng) * 2.0f - 1.0f) * x_extent;

    a->type               = AREA_TYPE_DYNAMIC_GATEWAY;
    a->gates_remaining    = n;
    a->gate_gap_half_w    = DYN_GATE_HOLE_HALF_W;
    a->gate_pad_z         = DYN_GATE_PAD_Z;
    // passage_mirror = "first wall already spawned" flag for this
    // area type. Cleared so the first wall's tick gets the booster
    // (if any is owed); set to 1 once it's spawned so subsequent
    // walls don't claim the booster slot.
    a->passage_mirror     = 0;
    float const thick = 2.0f * CUBE_GATE_HALF_D;
    // Layout: [pad][gate][pad][gate]...[gate][pad]
    // Lead-in and trailing pads are one inter-gate pad each, so the
    // empty run bracketing the area matches the gate-to-gate spacing
    // — no long dead stretch. Gates spawn at WORLD_Z_FAR_SPAWN, the
    // furthest spawn z, so a fresh gate is never placed behind a
    // leftover obstacle from the previous area.
    a->length_remaining_z = (float)(n + 1) * DYN_GATE_PAD_Z
                          + (float)n * thick;
    a->next_event_z       = DYN_GATE_PAD_Z;
    // boosters_owed deliberately untouched so a stale booster from
    // an earlier area carries forward into this one.
}

bool area_dynamic_gateway_tick(world_state_t* w, area_state_t* a, float dz) {
    a->length_remaining_z -= dz;
    a->next_event_z       -= dz;

    float const thick = 2.0f * CUBE_GATE_HALF_D;
    while (a->next_event_z <= 0.0f && a->gates_remaining > 0) {
        spawn_dyn_gate(w, a->gate_hole_x);

        // Pickup handling. Booster slot is reserved for the FIRST
        // wall in this area, and only if one is owed by the stage
        // scheduler — that one "free pass" through the cube
        // barrier is the gameplay reward. Every other hole gets a
        // Tri instead (Phase 6 rule: every wall not consuming a
        // booster gets a Tri). The Tri sits in the hole at
        // WORLD_Z_FAR_SPAWN, in front of the flipping cube which
        // is at WORLD_Z_FAR_SPAWN + DYN_GATE_CUBE_Z_OFFSET — so
        // the player collects the Tri before the cube reaches its
        // landed-and-blocking position.
        if (a->passage_mirror == 0) {
            if (a->boosters_owed > 0) {
                booster_spawn_at(w, a->gate_hole_x, WORLD_Z_FAR_SPAWN);
                a->boosters_owed--;
            } else {
                tri_spawn_at(w, a->gate_hole_x, WORLD_Z_FAR_SPAWN);
            }
            a->passage_mirror = 1;
        } else {
            tri_spawn_at(w, a->gate_hole_x, WORLD_Z_FAR_SPAWN);
        }

        a->gates_remaining--;
        a->next_event_z += a->gate_pad_z + thick;
    }

    return a->length_remaining_z <= 0.0f;
}
