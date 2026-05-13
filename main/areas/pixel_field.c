#include "areas/pixel_field.h"

#include "objects/booster.h"
#include "objects/cube.h"
#include "world.h"

#define AREA_PIXEL_MIN_LEN     116.0f
#define AREA_PIXEL_MAX_LEN     232.0f

// Pixel-field spawn cadence at stage 1. Subsequent stages shrink the
// interval via world_stage_interval_scale() down to a 0.5× floor.
#define PIXEL_INTERVAL_MIN     12.0f
#define PIXEL_INTERVAL_MAX     22.0f

void area_pixel_field_init(area_state_t* a, uint16_t stage, uint32_t* prng) {
    float const scale    = world_stage_interval_scale(stage);
    float const interval = (PIXEL_INTERVAL_MIN
                           + world_frand(prng) * (PIXEL_INTERVAL_MAX - PIXEL_INTERVAL_MIN))
                          * scale;
    a->type               = AREA_TYPE_PIXEL_FIELD;
    a->length_remaining_z = AREA_PIXEL_MIN_LEN
                          + world_frand(prng) * (AREA_PIXEL_MAX_LEN - AREA_PIXEL_MIN_LEN);
    a->next_event_z       = interval;
    a->gates_remaining    = 0;
    a->gate_gap_half_w    = 0.0f;
    a->gate_pad_z         = 0.0f;
    // `boosters_owed` deliberately left untouched so unfulfilled
    // boosters carry over from a previous area into this one.
}

bool area_pixel_field_tick(world_state_t* w, area_state_t* a, float dz) {
    a->length_remaining_z -= dz;
    a->next_event_z       -= dz;

    // When the stage scheduler has flagged a booster as owed, the
    // next event becomes a booster instead of a cube. Replacing
    // rather than co-spawning guarantees no overlap with adjacent
    // obstacles — the booster occupies a slot that would have held
    // a cube. One cube is "displaced" per booster, acceptable
    // density-wise.
    float const scale = world_stage_interval_scale(w->stage);
    while (a->next_event_z <= 0.0f && a->length_remaining_z > 0.0f) {
        if (a->boosters_owed > 0) {
            booster_spawn(w);
            a->boosters_owed--;
        } else {
            cube_spawn_pixel(w);
        }
        float const interval = (PIXEL_INTERVAL_MIN
                               + world_frand(&w->stage_prng)
                                 * (PIXEL_INTERVAL_MAX - PIXEL_INTERVAL_MIN))
                              * scale;
        a->next_event_z += interval;
    }

    return a->length_remaining_z <= 0.0f;
}
