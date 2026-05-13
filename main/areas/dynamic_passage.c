#include "areas/dynamic_passage.h"

#include <math.h>

#include "magicnumbers.h"
#include "objects/booster.h"
#include "objects/cube.h"
#include "objects/flipping_cube.h"
#include "objects/tri.h"
#include "world.h"

#define PASSAGE_FLIP_MIN              4
#define PASSAGE_FLIP_MAX              7

// Spacing between successive flipping cubes' centres along z.
// User spec: "space between them equal to their depth" — so centre-
// to-centre = (depth) + (gap = depth) = 2 × depth.
#define PASSAGE_FLIP_SPACING          (4.0f * FLIPPING_CUBE_HALF_D)  // = 2 × depth

// Continuous pixel-field clutter cadence inside the area. Tighter
// than the standalone pixel-field area so the non-revealed half of
// the playfield really looks impassable — the apparent corridor on
// the bait side is the only obvious through-path until the cubes
// flip. The actual rate is one cube per this many z-units of
// camera travel; the spawner skips cubes that would land in the
// wall-side safe lane.
#define PASSAGE_CLUTTER_INTERVAL      3.0f

// World-x of the flipping-cube column for each subtype. Pressed
// against the wall by half the cube's width so its outer face
// touches the inner wall surface.
#define PASSAGE_FLIP_X_NON_MIRRORED   (+(TRACK_HALF_WIDTH - FLIPPING_CUBE_HALF_W))
#define PASSAGE_FLIP_X_MIRRORED       (-(TRACK_HALF_WIDTH - FLIPPING_CUBE_HALF_W))

static inline float passage_flip_x(int mirrored) {
    return mirrored ? PASSAGE_FLIP_X_MIRRORED : PASSAGE_FLIP_X_NON_MIRRORED;
}

static inline int passage_roll_direction(int mirrored) {
    // Non-mirrored: cubes at right wall, roll LEFT (away from the
    // wall they're pressed against, into the bait corridor).
    // Mirrored: cubes at left wall, roll RIGHT. Both cases flip
    // the cubes off their starting wall.
    return mirrored ? +1 : -1;
}

void area_dynamic_passage_init(area_state_t* a, uint16_t stage, uint32_t* prng) {
    (void)stage;  // user spec: difficulty doesn't scale today

    // First PRNG draw picks the subtype (~50/50). Doing the coin
    // flip here keeps it deterministic from (level_seed, stage) and
    // out of the top-level picker, which sees this area as a single
    // slot.
    a->passage_mirror = (world_xorshift32(prng) & 1u) ? 1 : 0;

    int n = PASSAGE_FLIP_MIN
          + (int)(world_frand(prng) * (float)(PASSAGE_FLIP_MAX - PASSAGE_FLIP_MIN + 1));
    if (n > PASSAGE_FLIP_MAX) n = PASSAGE_FLIP_MAX;

    a->type               = AREA_TYPE_DYNAMIC_PASSAGE;
    a->gates_remaining    = n;
    a->gate_pad_z         = PASSAGE_FLIP_SPACING;  // store for clarity; not read elsewhere
    a->gate_gap_half_w    = 0.0f;
    // Length budget: enough for all N flipping cubes to spawn (one
    // every PASSAGE_FLIP_SPACING units of camera travel), plus a
    // short tail so the last cube has time to roll into place
    // before the next area's content starts arriving at the far
    // plane. Tail = PASSAGE_FLIP_SPACING.
    a->length_remaining_z = (float)n * PASSAGE_FLIP_SPACING;
    // First flipping cube spawns immediately (the area-start instant
    // is essentially "we've travelled 0 z into this area"; the cube
    // appears at the far plane on the first tick).
    a->next_event_z       = 0.0f;
    // First clutter cube also spawns immediately, then refills on
    // PASSAGE_CLUTTER_INTERVAL cadence.
    a->clutter_event_z    = 0.0f;
    // boosters_owed deliberately untouched so a stale booster from
    // an earlier area lands inside this one.
}

// Try a few uniform x draws; reject any that would land inside the
// wall-adjacent safe lane (so the player sees a continuous wall of
// clutter on that side instead of an alternate path). The reject
// rate is small (only ~16% of the track is the safe lane), so a
// handful of tries is plenty; on the rare exhaustion case we just
// spawn at the last-drawn x — a single clutter cube straddling the
// safe-lane edge is a far more acceptable failure mode than skipping
// the spawn entirely (which would visibly thin the clutter wall).
// Try to place a Tri in the clutter region — outside the safe lane
// (Phase 6 rule: "placed randomly in the pixel field area", which is
// the clutter side, not the bait lane) and not overlapping any
// existing obstacle at WORLD_Z_FAR_SPAWN. Skip silently if every
// candidate clashes — Tris are bonus pickups, not mandatory path.
static void emit_tri(world_state_t* w, area_state_t const* a) {
    float const flip_x       = passage_flip_x(a->passage_mirror);
    float const safe_radius  = FLIPPING_CUBE_HALF_W + GAME_TRI_HALF_W;
    float const x_extent     = TRACK_HALF_WIDTH - GAME_TRI_HALF_W - 0.5f;
    float const pad          = 1.5f * GAME_TRI_HALF_W;
    float const z_target     = WORLD_Z_FAR_SPAWN;
    float const z_lo         = z_target - GAME_TRI_HALF_W - pad;
    float const z_hi         = z_target + GAME_TRI_HALF_W + pad;

    for (int attempt = 0; attempt < 8; attempt++) {
        float const cand_x = (world_frand(&w->stage_prng) * 2.0f - 1.0f) * x_extent;
        // Reject if inside the safe lane — the Tri belongs in the
        // pixel-field clutter, not where the player will already be
        // steering naturally.
        if (fabsf(cand_x - flip_x) < safe_radius) continue;

        // Reject overlaps with active obstacles at this z.
        float const x_lo = cand_x - GAME_TRI_HALF_W - pad;
        float const x_hi = cand_x + GAME_TRI_HALF_W + pad;
        bool clash = false;
        for (int i = 0; i < WORLD_OBSTACLE_POOL_SIZE; i++) {
            obstacle_t const* o = &w->obstacles[i];
            if (!o->active) continue;
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
            tri_spawn_at(w, cand_x, z_target);
            return;
        }
    }
}

static void emit_clutter(world_state_t* w, area_state_t const* a) {
    float const x_extent = TRACK_HALF_WIDTH - CUBE_PIXEL_HALF_W;
    float const flip_x   = passage_flip_x(a->passage_mirror);
    // The safe lane is the column where the flipping cubes are now,
    // approximately ±FLIPPING_CUBE_HALF_W around flip_x. Reject
    // pixel-cube x values that would land in (or partially in) that
    // lane, taking the pixel cube's own half-width into account so
    // we don't visually graze the lane edge.
    float const reject_radius = FLIPPING_CUBE_HALF_W + CUBE_PIXEL_HALF_W;

    float x = 0.0f;
    for (int attempt = 0; attempt < 8; attempt++) {
        x = (world_frand(&w->stage_prng) * 2.0f - 1.0f) * x_extent;
        if (fabsf(x - flip_x) >= reject_radius) break;
    }
    cube_spawn_pixel_at(w, x, WORLD_Z_FAR_SPAWN);
}

bool area_dynamic_passage_tick(world_state_t* w, area_state_t* a, float dz) {
    a->length_remaining_z -= dz;
    a->next_event_z       -= dz;
    a->clutter_event_z    -= dz;

    // --- Flipping-cube emissions (one every PASSAGE_FLIP_SPACING) ---
    // We also consume any owed booster here, dropping it half a
    // spacing back from the just-spawned cube along the wall lane.
    // That places it dead-centre in the z-gap between this cube and
    // the next — the player sees boosters strung along the safe
    // lane after the flips finish.
    while (a->next_event_z <= 0.0f && a->gates_remaining > 0) {
        float const flip_x = passage_flip_x(a->passage_mirror);
        int   const dir    = passage_roll_direction(a->passage_mirror);
        flipping_cube_spawn(w, flip_x, WORLD_Z_FAR_SPAWN, dir);

        if (a->boosters_owed > 0) {
            // Booster sits in the gap behind this cube (closer to the
            // camera by half a spacing). That puts it dead-centre
            // between this cube's near face and the next cube's far
            // face once both are spawned.
            booster_spawn_at(w, flip_x, WORLD_Z_FAR_SPAWN - 0.5f * PASSAGE_FLIP_SPACING);
            a->boosters_owed--;
        }

        // Phase 6: one Tri per flipping cube, placed in the clutter
        // region (outside the safe lane). Spawned at the same z as
        // the cube — the Tri is visible in the clutter as the cube
        // appears, giving the player a visual reward target tied to
        // each cube. Co-spawned rather than on its own cadence so
        // count tracks the cube count exactly.
        emit_tri(w, a);

        a->gates_remaining--;
        a->next_event_z += PASSAGE_FLIP_SPACING;
    }

    // --- Pixel-field clutter emissions ---
    // Independent timer so the clutter stream stays continuous
    // regardless of the flipping cadence. We keep emitting clutter
    // even after the flipping cubes are exhausted, until the area's
    // length budget runs out — that way the tail of the area still
    // reads as "thick pixel field on one side, open lane on the
    // wall side" while the last cube finishes its roll.
    while (a->clutter_event_z <= 0.0f && a->length_remaining_z > 0.0f) {
        emit_clutter(w, a);
        a->clutter_event_z += PASSAGE_CLUTTER_INTERVAL;
    }

    return a->length_remaining_z <= 0.0f;
}
