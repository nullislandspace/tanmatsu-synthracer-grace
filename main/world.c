#include "world.h"

#include "areas/big_blocks.h"
#include "areas/bridges.h"
#include "areas/dynamic_gateway.h"
#include "areas/dynamic_passage.h"
#include "areas/gateways.h"
#include "areas/pixel_field.h"
#include "areas/rest.h"
#include "magicnumbers.h"
#include "objects/booster.h"
#include "objects/tri.h"
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
static uint32_t mix_stage_seed(uint32_t level_seed, uint16_t stage) {
    uint32_t s = level_seed ^ ((uint32_t)stage * 0x9E3779B9u);
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s ? s : 1u;
}

float world_lerp_by_stage(uint16_t stage, float at_one, float at_ten) {
    if (stage <= 1)  return at_one;
    if (stage >= 10) return at_ten;
    float const t = (float)(stage - 1) / 9.0f;
    return at_one + (at_ten - at_one) * t;
}

float world_stage_interval_scale(uint16_t stage) {
    float s = 1.0f - 0.05f * (float)((int)stage - 1);
    if (s < 0.5f) s = 0.5f;
    if (s > 1.0f) s = 1.0f;
    return s;
}

bool world_find_free_x(world_state_t const* w, uint32_t* prng,
                       float z_target, float half_w, float pad,
                       int max_tries, float* out_x) {
    // Candidate range inset from the wall by half_w + pad so the
    // Tri's full footprint stays inside the playfield by at least
    // `pad`. Z range for overlap testing is half_w + pad either
    // side of z_target (same breathing budget on the depth axis).
    float const inset    = half_w + pad;
    float const x_extent = TRACK_HALF_WIDTH - inset;
    if (x_extent <= 0.0f || max_tries <= 0) return false;

    float const z_lo = z_target - half_w - pad;
    float const z_hi = z_target + half_w + pad;

    for (int attempt = 0; attempt < max_tries; attempt++) {
        float const cand_x = (world_frand(prng) * 2.0f - 1.0f) * x_extent;
        float const x_lo   = cand_x - half_w - pad;
        float const x_hi   = cand_x + half_w + pad;

        bool clash = false;
        for (int i = 0; i < WORLD_OBSTACLE_POOL_SIZE; i++) {
            obstacle_t const* o = &w->obstacles[i];
            if (!o->active) continue;

            // AABB overlap on (x, z) with breathing-room pad on both.
            float const o_x_lo = o->x_world - o->half_w;
            float const o_x_hi = o->x_world + o->half_w;
            float const o_z_lo = o->z_world - o->half_d;
            float const o_z_hi = o->z_world + o->half_d;

            if (x_hi > o_x_lo && x_lo < o_x_hi &&
                z_hi > o_z_lo && z_lo < o_z_hi) {
                clash = true;
                break;
            }
        }
        if (!clash) {
            *out_x = cand_x;
            return true;
        }
    }
    return false;
}

// --- Area state machine --------------------------------------------

// Stage range (inclusive on both ends) at which each area type is
// allowed to be picked. Updated as new areas land that should
// unlock later or retire earlier (e.g. "pixel field is too easy past
// stage 10"). Today every area has [1, 0xFF] so the picker's retry
// loop never actually rejects — but the gating data is in one place
// and the loop honours it the moment any range tightens.
//
// 0xFFFF is the "never expires" sentinel — stage is a uint16_t so
// 65535 is effectively unreachable.
static bool area_is_applicable(area_type_t t, uint16_t stage) {
    uint16_t min_stage, max_stage;
    switch (t) {
        case AREA_TYPE_PIXEL_FIELD:     min_stage = 1; max_stage = 0xFFFF; break;
        case AREA_TYPE_GATEWAYS:        min_stage = 1; max_stage = 0xFFFF; break;
        case AREA_TYPE_BIG_BLOCKS:      min_stage = 1; max_stage = 0xFFFF; break;
        case AREA_TYPE_BRIDGES:         min_stage = 1; max_stage = 0xFFFF; break;
        case AREA_TYPE_DYNAMIC_PASSAGE: min_stage = 2; max_stage = 5;      break;
        case AREA_TYPE_DYNAMIC_GATEWAY: min_stage = 3; max_stage = 6;      break;
        case AREA_TYPE_REST:            min_stage = 1; max_stage = 0xFFFF; break;  // never picked anyway
        default:                        min_stage = 1; max_stage = 0xFFFF; break;
    }
    return stage >= min_stage && stage <= max_stage;
}

// Pick a random applicable area type for `stage`. Loops on rejection
// so the gating mechanism scales — when a future area lands with
// min_stage > 1, low-stage runs simply re-roll until they land on
// something that's currently unlocked. The attempt cap prevents a
// pathological configuration (every candidate gated above the
// current stage) from spinning; it falls through to AREA_TYPE_PIXEL_FIELD
// which is guaranteed unlocked from stage 1.
static area_type_t pick_area_type(uint16_t stage, uint32_t* prng) {
    static area_type_t const candidates[] = {
        AREA_TYPE_PIXEL_FIELD,
        AREA_TYPE_GATEWAYS,
        AREA_TYPE_BIG_BLOCKS,
        AREA_TYPE_BRIDGES,
        AREA_TYPE_DYNAMIC_PASSAGE,
        AREA_TYPE_DYNAMIC_GATEWAY,
    };
    int const n = (int)(sizeof(candidates) / sizeof(candidates[0]));

    for (int attempt = 0; attempt < 32; attempt++) {
        uint32_t    const r = world_xorshift32(prng);
        area_type_t const t = candidates[r % (uint32_t)n];
        if (area_is_applicable(t, stage)) return t;
    }
    return AREA_TYPE_PIXEL_FIELD;
}

static void start_next_area(world_state_t* w) {
    area_type_t t;
    if (w->forced_next_area_type >= 0) {
        t = (area_type_t)w->forced_next_area_type;
        w->forced_next_area_type = -1;  // consume override
    } else {
        t = pick_area_type(w->stage, &w->stage_prng);
    }
    switch (t) {
        case AREA_TYPE_PIXEL_FIELD:     area_pixel_field_init    (&w->area, w->stage, &w->stage_prng); break;
        case AREA_TYPE_GATEWAYS:        area_gateways_init       (&w->area, w->stage, &w->stage_prng); break;
        case AREA_TYPE_BIG_BLOCKS:      area_big_blocks_init     (&w->area, w->stage, &w->stage_prng); break;
        case AREA_TYPE_BRIDGES:         area_bridges_init        (&w->area, w->stage, &w->stage_prng); break;
        case AREA_TYPE_DYNAMIC_PASSAGE: area_dynamic_passage_init(&w->area, w->stage, &w->stage_prng); break;
        case AREA_TYPE_DYNAMIC_GATEWAY: area_dynamic_gateway_init(&w->area, w->stage, &w->stage_prng); break;
        case AREA_TYPE_REST:            area_rest_init           (&w->area, WORLD_REST_LENGTH_Z);       break;
    }
}

static bool area_tick(world_state_t* w, float dz) {
    area_state_t* a = &w->area;
    switch (a->type) {
        case AREA_TYPE_PIXEL_FIELD:     return area_pixel_field_tick    (w, a, dz);
        case AREA_TYPE_BIG_BLOCKS:      return area_big_blocks_tick     (w, a, dz);
        case AREA_TYPE_GATEWAYS:        return area_gateways_tick       (w, a, dz);
        case AREA_TYPE_BRIDGES:         return area_bridges_tick        (w, a, dz);
        case AREA_TYPE_DYNAMIC_PASSAGE: return area_dynamic_passage_tick(w, a, dz);
        case AREA_TYPE_DYNAMIC_GATEWAY: return area_dynamic_gateway_tick(w, a, dz);
        case AREA_TYPE_REST:            return area_rest_tick           (w, a, dz);
    }
    return false;
}

// Phase 6: spawn the rest area's Tri + booster S-curve. 10 Tris
// and one booster sampled along a quadratic Bézier whose control
// points are picked so:
//   - the start (t=0) sits midway between the centreline and one
//     of the two walls (`start_x = ±0.5 × TRACK_HALF_WIDTH`),
//     direction picked by the PRNG;
//   - the curve's x-tangent at t=0 is zero — the curve "starts
//     pointing straight ahead" — by setting the Bézier control
//     point's x equal to the start's x;
//   - the end (t=1) is the mirror of the start across the
//     centreline, so the curve sweeps from one side of the track
//     to the other.
// z linearly spans most of the rest area's length, so the 11
// pickups are visually spread out as the player traverses.
//
// The 11th sample (`i == 10`) is a booster, satisfying
// GAME_BOOSTERS_PER_REST without a separate scatter spawn.
//
// Math: with P0.x = P1.x = start_x and P2.x = end_x, the Bézier
// formula `(1-t)² P0 + 2(1-t)t P1 + t² P2` simplifies to
//   x(t) = start_x · (1 − t²) + end_x · t²
// which is the line we use.
static void spawn_rest_tri_curve(world_state_t* w) {
    float const half_offset = 0.5f * TRACK_HALF_WIDTH;
    bool  const start_right = (world_xorshift32(&w->stage_prng) & 1u) != 0u;
    float const start_x     = start_right ? +half_offset : -half_offset;
    float const end_x       = -start_x;

    // Spread the 11 samples over 70% of the rest area's length
    // starting at WORLD_Z_FAR_SPAWN. Trailing 30% gives time for
    // the player to traverse the curve before the next stage's
    // first area begins spawning at the far plane.
    float const z_first = WORLD_Z_FAR_SPAWN;
    float const z_last  = WORLD_Z_FAR_SPAWN + WORLD_REST_LENGTH_Z * 0.7f;

    for (int i = 0; i <= 10; i++) {
        float const t  = (float)i / 10.0f;
        float const x  = start_x * (1.0f - t * t) + end_x * (t * t);
        float const z  = z_first + (z_last - z_first) * t;
        if (i < 10) {
            tri_spawn_at(w, x, z);
        } else {
            booster_spawn_at(w, x, z);
        }
    }
}

static void start_stage(world_state_t* w, uint16_t stage) {
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

void world_force_next_area(world_state_t* w, area_type_t t) {
    w->forced_next_area_type = (int)t;
    // End the current area's budget on this frame so the next
    // world-advance pass triggers the area-done branch and the
    // override is consumed immediately.
    w->area.length_remaining_z = 0.0f;
}

void world_init(world_state_t* w, uint32_t seed) {
    for (int i = 0; i < WORLD_OBSTACLE_POOL_SIZE; i++) {
        w->obstacles[i].active = false;
    }
    w->forced_next_area_type = -1;
    w->level_seed = seed ? seed : 1u;
    // Start each wall's far cursor at half a segment so the first
    // segment is centred at z = WALL_SEGMENT_HALF_D (= 1.5), running
    // from z=0 to z=3 — between the first two drawn grid stripes.
    w->right_wall_far_z = WALL_SEGMENT_HALF_D;
    w->left_wall_far_z  = WALL_SEGMENT_HALF_D;
    wall_top_up(w, &w->right_wall_far_z, WALL_X_RIGHT);
    wall_top_up(w, &w->left_wall_far_z,  WALL_X_LEFT);
    // Begin with a short pre-stage-1 rest area — just one screen
    // depth (WORLD_Z_FAR_SPAWN) — so the run opens with a brief
    // clear lead-in rather than a long empty crawl, and the
    // "Stage: 1" banner is visible for its whole duration.
    // w->stage stays at 0 during this rest; when the rest area
    // finishes, the standard area-done handler fires
    // `start_stage(0 + 1)` which sets up stage 1 normally. The rest
    // dump still spawns its quota of boosters so the player gets a
    // greeting boost before the first stage proper.
    w->stage             = 0;
    w->stage_z_remaining = 0.0f;          // already "at stage end" — rest end will fire start_stage(1)
    w->stage_prng        = mix_stage_seed(w->level_seed, 0);
    w->area.boosters_owed = 0;
    for (int i = 0; i < GAME_BOOSTERS_PER_STAGE; i++) {
        w->booster_due_at_progress[i] = -1.0f;  // nothing scheduled yet
    }
    area_rest_init(&w->area, WORLD_Z_FAR_SPAWN);
    for (int i = 0; i < GAME_BOOSTERS_PER_REST; i++) {
        booster_spawn(w);
    }
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
            start_stage(w, (uint16_t)(w->stage + 1));
        } else if (w->stage_z_remaining <= 0.0f) {
            // Between-stage rest area. This is the path that runs
            // *after* stage 1 onwards — the pre-stage-1 lead-in at
            // world_init() takes a different path and is
            // deliberately excluded from the Tri curve below.
            //
            // Capture leftovers before area_rest_init overwrites
            // most of the area state (it doesn't touch
            // boosters_owed, but read it explicitly so the intent
            // is local).
            int const leftover_boosters = w->area.boosters_owed;
            area_rest_init(&w->area, WORLD_REST_LENGTH_Z);
            w->area.boosters_owed = 0;

            // Phase 6: spawn the 10-Tri + 1-booster quadratic-Bezier
            // S-curve through the rest area. The booster on the
            // curve fulfils GAME_BOOSTERS_PER_REST exactly.
            spawn_rest_tri_curve(w);

            // Leftover boosters carried over from the prior stage
            // can't fit the curve cleanly — drop them on the floor
            // at random x as a "make-good" so the player isn't
            // robbed of accrued bonus pickups.
            for (int i = 0; i < leftover_boosters; i++) {
                booster_spawn(w);
            }
        } else {
            start_next_area(w);
        }
    }
}
