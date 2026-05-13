#include "areas/rest.h"

#include "world.h"

void area_rest_init(area_state_t* a) {
    a->type               = AREA_TYPE_REST;
    a->length_remaining_z = WORLD_REST_LENGTH_Z;
    a->next_event_z       = WORLD_REST_LENGTH_Z;
    a->gates_remaining    = 0;
    a->gate_gap_half_w    = 0.0f;
    a->gate_pad_z         = 0.0f;
    // `boosters_owed` is left untouched on init so the rest-entry
    // handler in world.c can read it and dump any leftovers.
}

bool area_rest_tick(world_state_t* w, area_state_t* a, float dz) {
    (void)w;
    a->length_remaining_z -= dz;
    a->next_event_z       -= dz;
    return a->length_remaining_z <= 0.0f;
}
