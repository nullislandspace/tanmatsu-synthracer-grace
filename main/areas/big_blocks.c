#include "areas/big_blocks.h"

#include "magicnumbers.h"
#include "objects/booster.h"
#include "objects/cube.h"
#include "objects/tri.h"
#include "world.h"

#define AREA_BIG_MIN_LEN       116.0f
#define AREA_BIG_MAX_LEN       232.0f

// Sparser than pixel field — the cubes are 4× the cross-section and
// would wall off the track at pixel-field density. Same per-stage
// scaling via world_stage_interval_scale().
#define BIG_INTERVAL_MIN       20.0f
#define BIG_INTERVAL_MAX       35.0f

// Tri cadence — *equal* to the cube cadence per the Phase 6 design
// ("Tri count = equal to the number of big-cube blocks"). Same
// interval bounds; separate timer on `tri_event_z` so the two
// streams don't share a slot.
#define TRI_REJECT_PAD         (1.5f * GAME_TRI_HALF_W)

void area_big_blocks_init(area_state_t* a, uint16_t stage, uint32_t* prng) {
    float const scale    = world_stage_interval_scale(stage);
    float const interval = (BIG_INTERVAL_MIN
                           + world_frand(prng) * (BIG_INTERVAL_MAX - BIG_INTERVAL_MIN))
                          * scale;
    float const tri_interval = (BIG_INTERVAL_MIN
                               + world_frand(prng) * (BIG_INTERVAL_MAX - BIG_INTERVAL_MIN))
                              * scale;
    a->type               = AREA_TYPE_BIG_BLOCKS;
    a->length_remaining_z = AREA_BIG_MIN_LEN
                          + world_frand(prng) * (AREA_BIG_MAX_LEN - AREA_BIG_MIN_LEN);
    a->next_event_z       = interval;
    a->tri_event_z        = tri_interval;
    a->gates_remaining    = 0;
    a->gate_gap_half_w    = 0.0f;
    a->gate_pad_z         = 0.0f;
}

bool area_big_blocks_tick(world_state_t* w, area_state_t* a, float dz) {
    a->length_remaining_z -= dz;
    a->next_event_z       -= dz;
    a->tri_event_z        -= dz;

    // Same booster-displaces-cube rule as pixel field.
    float const scale = world_stage_interval_scale(w->stage);
    while (a->next_event_z <= 0.0f && a->length_remaining_z > 0.0f) {
        if (a->boosters_owed > 0) {
            booster_spawn(w);
            a->boosters_owed--;
        } else {
            cube_spawn_big(w);
        }
        float const interval = (BIG_INTERVAL_MIN
                               + world_frand(&w->stage_prng)
                                 * (BIG_INTERVAL_MAX - BIG_INTERVAL_MIN))
                              * scale;
        a->next_event_z += interval;
    }

    // Tri cadence — parallel to the cube stream, rejection-sampled
    // against the live pool so Tris never embed in big blocks or
    // each other. Big blocks are sparse so rejection rarely fires;
    // when it does we skip silently (8 retries, then give up).
    while (a->tri_event_z <= 0.0f && a->length_remaining_z > 0.0f) {
        float tri_x = 0.0f;
        if (world_find_free_x(w, &w->stage_prng,
                              WORLD_Z_FAR_SPAWN, GAME_TRI_HALF_W,
                              TRI_REJECT_PAD, 8, &tri_x)) {
            tri_spawn_at(w, tri_x, WORLD_Z_FAR_SPAWN);
        }
        float const tri_interval = (BIG_INTERVAL_MIN
                                   + world_frand(&w->stage_prng)
                                     * (BIG_INTERVAL_MAX - BIG_INTERVAL_MIN))
                                  * scale;
        a->tri_event_z += tri_interval;
    }

    return a->length_remaining_z <= 0.0f;
}
