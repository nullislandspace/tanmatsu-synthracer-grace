#include "areas/rest.h"

#include <math.h>

#include "objects/rest_pillar.h"
#include "objects/wall.h"   // WALL_SEGMENT_LEN / WALL_SEGMENT_HALF_D
#include "world.h"

// Centre-to-centre spacing of the green border-wall posts that flag
// the rest stretch. Three wall segments (= 9 u) apart, matching the
// bridges area's pillar cadence so the rest area reads as the same
// visual family.
#define REST_PILLAR_PAD_Z   (WALL_SEGMENT_LEN * 3.0f)

// Snap a target spawn z down to the nearest wall-segment centre at
// or below `target`, so the posts land cleanly on the wall grid
// (same alignment trick the bridges area uses).
static float snap_to_wall_segment(float target) {
    int const n = (int)floorf((target - WALL_SEGMENT_HALF_D) / WALL_SEGMENT_LEN);
    return (float)n * WALL_SEGMENT_LEN + WALL_SEGMENT_HALF_D;
}

void area_rest_init(area_state_t* a, float length_z) {
    a->type               = AREA_TYPE_REST;
    a->length_remaining_z = length_z;
    a->next_event_z       = 0.0f;   // first post pair spawns on the first tick
    a->gates_remaining    = 0;
    a->gate_gap_half_w    = 0.0f;
    a->gate_pad_z         = 0.0f;
    // `boosters_owed` is left untouched on init so the rest-entry
    // handler in world.c can read it and dump any leftovers.
}

bool area_rest_tick(world_state_t* w, area_state_t* a, float dz) {
    a->length_remaining_z -= dz;
    a->next_event_z       -= dz;

    // Line the side walls with green marker posts at a fixed
    // wall-segment-aligned cadence for the whole rest stretch, so
    // it's visually obvious the player is in the between-stage
    // breather. Posts are cosmetic — they sit on the wall tops,
    // outside the ship's reachable x, so they never collide.
    float const spawn_z = snap_to_wall_segment(WORLD_Z_FAR_SPAWN);
    while (a->next_event_z <= 0.0f) {
        rest_pillar_pair_spawn(w, spawn_z);
        a->next_event_z += REST_PILLAR_PAD_Z;
    }

    return a->length_remaining_z <= 0.0f;
}
