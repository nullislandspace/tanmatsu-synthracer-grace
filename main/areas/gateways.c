#include "areas/gateways.h"

#include "objects/booster.h"
#include "objects/cube.h"
#include "objects/tri.h"
#include "world.h"

// Ship full collision width is 2 * SHIP_COLLISION_HALF_W = 0.56 u
// (kept in sync with game.h via a literal here to avoid an awkward
// include cycle for a single constant). Opening scales linearly
// from 3× ship width at stage 1 down to 1.5× at stage 10 and clamps
// from there — that's where the difficulty curve lives.
// Inter-gate / lead-in / trailing pad is a fixed 50 u for all
// stages, sized so even back-to-back hard-left → hard-right
// alignment shifts are physically reachable at cruise speed.
#define SHIP_FULL_WIDTH         (2.0f * 0.28f)
#define GATEWAY_OPENING_STAGE1  (3.0f * SHIP_FULL_WIDTH)
#define GATEWAY_OPENING_STAGE10 (1.5f * SHIP_FULL_WIDTH)
#define GATEWAY_PAD_Z           50.0f
#define GATEWAY_COUNT_MIN       1
#define GATEWAY_COUNT_MAX       5

// Settling distance at the start of a gateway area. The previous
// area can spawn its last obstacle right up to the area boundary,
// placing it at camera-relative z = WORLD_Z_FAR_SPAWN. Waiting one
// full far-plane distance of camera travel guarantees that obstacle
// has crossed the camera and despawned before the gateway's
// alignment pad starts ticking — so the whole gateway area, lead-in
// included, is free of drifting leftovers and the player only has
// the gate itself to focus on. Reads visually as a deliberate
// breath before the alignment puzzle.
#define GATEWAY_SETTLE_Z        WORLD_Z_FAR_SPAWN

// Spawn a gateway: two cube slabs flanking a central gap of width
// `2 * half_gap`. The gap centre is picked so that both slabs are
// entirely inside the playfield. If `with_booster` is true, drops
// a booster centred in the gap at the same z; otherwise drops a
// Tri there instead (Phase 6: every gateway hole holds *something*
// pickup-able — booster takes priority, Tri fills the rest).
static void spawn_gate(world_state_t* w, float half_gap, bool with_booster) {
    float const gap_centre_extent = TRACK_HALF_WIDTH - half_gap;
    float       gap_x             = 0.0f;
    if (gap_centre_extent > 0.0f) {
        gap_x = (world_frand(&w->stage_prng) * 2.0f - 1.0f) * gap_centre_extent;
    }
    float const left_inner   = gap_x - half_gap;
    float const left_outer   = -TRACK_HALF_WIDTH;
    float const right_inner  = gap_x + half_gap;
    float const right_outer  = TRACK_HALF_WIDTH;
    float const left_half_w  = (left_inner - left_outer) * 0.5f;
    float const left_centre  = (left_inner + left_outer) * 0.5f;
    float const right_half_w = (right_outer - right_inner) * 0.5f;
    float const right_centre = (right_outer + right_inner) * 0.5f;
    cube_spawn_gate_slab(w, left_centre,  WORLD_Z_FAR_SPAWN, left_half_w);
    cube_spawn_gate_slab(w, right_centre, WORLD_Z_FAR_SPAWN, right_half_w);
    if (with_booster) {
        booster_spawn_at(w, gap_x, WORLD_Z_FAR_SPAWN);
    } else {
        tri_spawn_at(w, gap_x, WORLD_Z_FAR_SPAWN);
    }
}

void area_gateways_init(area_state_t* a, uint16_t stage, uint32_t* prng) {
    int n = GATEWAY_COUNT_MIN
          + (int)(world_frand(prng) * (float)(GATEWAY_COUNT_MAX - GATEWAY_COUNT_MIN + 1));
    if (n > GATEWAY_COUNT_MAX) n = GATEWAY_COUNT_MAX;
    float const gap   = world_lerp_by_stage(stage, GATEWAY_OPENING_STAGE1, GATEWAY_OPENING_STAGE10);
    float const pad   = GATEWAY_PAD_Z;
    float const thick = 2.0f * CUBE_GATE_HALF_D;
    a->type               = AREA_TYPE_GATEWAYS;
    a->gates_remaining    = n;
    a->gate_gap_half_w    = gap * 0.5f;
    a->gate_pad_z         = pad;
    // [settle][pad][gate][pad][gate]...[gate][pad]
    //  ^         ^                              ^
    //  prev-area drain   alignment              trailing
    //                    lead-in                pad
    // = settle + (n+1) pads + n gate thicknesses.
    a->length_remaining_z = GATEWAY_SETTLE_Z + (float)(n + 1) * pad + (float)n * thick;
    a->next_event_z       = GATEWAY_SETTLE_Z + pad;  // first gate after settle + one lead-in pad
}

bool area_gateways_tick(world_state_t* w, area_state_t* a, float dz) {
    a->length_remaining_z -= dz;
    a->next_event_z       -= dz;

    // If a booster is owed, it rides along inside the next gate's
    // gap — the player has to steer to the gap anyway to dodge the
    // gate, so picking up the booster is the default path.
    float const thick = 2.0f * CUBE_GATE_HALF_D;
    while (a->next_event_z <= 0.0f && a->gates_remaining > 0) {
        bool const with_booster = a->boosters_owed > 0;
        spawn_gate(w, a->gate_gap_half_w, with_booster);
        if (with_booster) a->boosters_owed--;
        a->gates_remaining--;
        a->next_event_z += a->gate_pad_z + thick;
    }

    return a->length_remaining_z <= 0.0f;
}
