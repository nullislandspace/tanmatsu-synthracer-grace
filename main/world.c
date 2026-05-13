#include "world.h"

#include "areas/big_blocks.h"
#include "areas/gateways.h"
#include "areas/pixel_field.h"
#include "areas/rest.h"
#include "objects/booster.h"
#include "objects/wall.h"

// --- PRNG / stage helpers ------------------------------------------
// xorshift32 — small, fast, deterministic from a seed. Used both for
// per-stage content (mixed from level_seed + stage index) and for
// the area picker's uniform draws.

uint32_t world_xorshift32(uint32_t* s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x ? x : 1u;
    return *s;
}

float world_frand(uint32_t* s) {
    return (float)world_xorshift32(s) / 4294967296.0f;
}

// Stage seed derivation. Mixes the run's level seed with the stage
// index so each stage's content is fully determined by
// (level_seed, stage) — replaying the same seed reproduces every
// stage identically.
static uint32_t mix_stage_seed(uint32_t level_seed, uint8_t stage) {
    uint32_t s = level_seed ^ ((uint32_t)stage * 0x9E3779B9u);
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s ? s : 1u;
}

float world_lerp_by_stage(uint8_t stage, float at_one, float at_ten) {
    if (stage <= 1)  return at_one;
    if (stage >= 10) return at_ten;
    float const t = (float)(stage - 1) / 9.0f;
    return at_one + (at_ten - at_one) * t;
}

float world_stage_interval_scale(uint8_t stage) {
    float s = 1.0f - 0.05f * (float)((int)stage - 1);
    if (s < 0.5f) s = 0.5f;
    if (s > 1.0f) s = 1.0f;
    return s;
}

// --- Area state machine --------------------------------------------

// Pick a random applicable area type for this stage. All three
// obstacle area types unlock at stage 1, so today the picker is a
// uniform draw. New area types add an entry gated by their min_stage.
static area_type_t pick_area_type(uint8_t stage, uint32_t* prng) {
    area_type_t candidates[3];
    int         n = 0;
    candidates[n++] = AREA_TYPE_PIXEL_FIELD;
    candidates[n++] = AREA_TYPE_GATEWAYS;
    candidates[n++] = AREA_TYPE_BIG_BLOCKS;
    (void)stage;
    uint32_t const r = world_xorshift32(prng);
    return candidates[r % (uint32_t)n];
}

static void start_next_area(world_state_t* w) {
    area_type_t const t = pick_area_type(w->stage, &w->stage_prng);
    switch (t) {
        case AREA_TYPE_PIXEL_FIELD: area_pixel_field_init(&w->area, w->stage, &w->stage_prng); break;
        case AREA_TYPE_GATEWAYS:    area_gateways_init   (&w->area, w->stage, &w->stage_prng); break;
        case AREA_TYPE_BIG_BLOCKS:  area_big_blocks_init (&w->area, w->stage, &w->stage_prng); break;
        case AREA_TYPE_REST:        area_rest_init       (&w->area);                           break;
    }
}

static bool area_tick(world_state_t* w, float dz) {
    area_state_t* a = &w->area;
    switch (a->type) {
        case AREA_TYPE_PIXEL_FIELD: return area_pixel_field_tick(w, a, dz);
        case AREA_TYPE_BIG_BLOCKS:  return area_big_blocks_tick (w, a, dz);
        case AREA_TYPE_GATEWAYS:    return area_gateways_tick   (w, a, dz);
        case AREA_TYPE_REST:        return area_rest_tick       (w, a, dz);
    }
    return false;
}

static void start_stage(world_state_t* w, uint8_t stage) {
    w->stage             = stage;
    w->stage_z_remaining = WORLD_STAGE_LENGTH_Z;
    w->stage_prng        = mix_stage_seed(w->level_seed, stage);
    // Clear any unspent owed-counter from the previous stage. The
    // rest-entry handler should already have drained it, but reset
    // defensively so a logic bug doesn't accumulate across runs.
    w->area.boosters_owed = 0;

    // Schedule the stage's boosters: divide the stage length into N
    // equal segments and place one booster in each segment at a
    // jittered position (0.25..0.75 of the segment). Roughly equal
    // spacing, deterministic from the stage seed.
    float const segment = WORLD_STAGE_LENGTH_Z / (float)GAME_BOOSTERS_PER_STAGE;
    for (int i = 0; i < GAME_BOOSTERS_PER_STAGE; i++) {
        float const jitter = 0.25f + 0.5f * world_frand(&w->stage_prng);
        w->booster_due_at_progress[i] = ((float)i + jitter) * segment;
    }

    start_next_area(w);
}

// --- Public API ----------------------------------------------------

void world_init(world_state_t* w, uint32_t seed) {
    for (int i = 0; i < WORLD_OBSTACLE_POOL_SIZE; i++) {
        w->obstacles[i].active = false;
    }
    w->level_seed = seed ? seed : 1u;
    // Start each wall's far cursor at half a segment so the first
    // segment is centred at z = WALL_SEGMENT_HALF_D (= 1.5), running
    // from z=0 to z=3 — between the first two drawn grid stripes.
    w->right_wall_far_z = WALL_SEGMENT_HALF_D;
    w->left_wall_far_z  = WALL_SEGMENT_HALF_D;
    wall_top_up(w, &w->right_wall_far_z, WALL_X_RIGHT);
    wall_top_up(w, &w->left_wall_far_z,  WALL_X_LEFT);
    // Stage 1 starts immediately. The first area's lead-in plus the
    // far-plane spawn distance gives the player ~5 s of clear track
    // before the first obstacle reaches them — that's our "starting
    // rest" without needing an explicit rest area up front.
    start_stage(w, 1);
}

void world_advance(world_state_t* w, float dt, float speed_z, float cam_x) {
    float const dz = speed_z * dt;

    // Pool sweep. Three things per active obstacle: advance z by dz,
    // fire optional physics callback (so self-moving objects update
    // before collision sees them), then despawn (with cleanup) if
    // the back edge has crossed the near plane.
    for (int i = 0; i < WORLD_OBSTACLE_POOL_SIZE; i++) {
        obstacle_t* o = &w->obstacles[i];
        if (!o->active) continue;
        o->z_world -= dz;
        if (o->physics) {
            o->physics(o, w, dt, cam_x);
        }
        if (o->z_world + o->half_d < WORLD_Z_NEAR_DESPAWN) {
            obstacle_despawn(o);
        }
    }

    // Slide the wall cursors with the camera and refill anything
    // that's drifted into the spawn window.
    w->right_wall_far_z -= dz;
    w->left_wall_far_z  -= dz;
    wall_top_up(w, &w->right_wall_far_z, WALL_X_RIGHT);
    wall_top_up(w, &w->left_wall_far_z,  WALL_X_LEFT);

    // Drive the stage / area state machine.
    w->stage_z_remaining -= dz;
    bool const area_done = area_tick(w, dz);

    // Booster scheduler — decides *when* a booster is due. Each
    // scheduled progress mark, once crossed, bumps the active area's
    // `boosters_owed` counter; the area's tick (above) consumes it
    // and decides *where* the booster goes. Rest areas skip this
    // loop — their booster handling is the dump on entry, just below.
    if (w->area.type != AREA_TYPE_REST) {
        float const stage_progress = WORLD_STAGE_LENGTH_Z - w->stage_z_remaining;
        for (int i = 0; i < GAME_BOOSTERS_PER_STAGE; i++) {
            if (w->booster_due_at_progress[i] >= 0.0f
                && stage_progress >= w->booster_due_at_progress[i]) {
                w->area.boosters_owed++;
                w->booster_due_at_progress[i] = -1.0f;
            }
        }
    }

    if (area_done) {
        if (w->area.type == AREA_TYPE_REST) {
            start_stage(w, (uint8_t)(w->stage + 1));
        } else if (w->stage_z_remaining <= 0.0f) {
            // Capture leftovers before area_rest_init overwrites
            // most of the area state (it doesn't touch
            // boosters_owed, but read it explicitly so the intent
            // is local).
            int const leftover_boosters = w->area.boosters_owed;
            area_rest_init(&w->area);
            int const total = leftover_boosters + GAME_BOOSTERS_PER_REST;
            for (int i = 0; i < total; i++) {
                booster_spawn(w);
            }
            w->area.boosters_owed = 0;
        } else {
            start_next_area(w);
        }
    }
}
