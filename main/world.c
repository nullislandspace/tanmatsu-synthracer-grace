#include "world.h"

// Track geometry. The track is `2 * TRACK_HALF_WIDTH` world units wide,
// centred on x=0. The playfield is intentionally much wider than the
// viewport at ship-z (~±2.3 visible units) — the camera pans laterally
// with the ship so the player can move sideways to find clear lanes,
// matching Race The Sun's behavior.
#define TRACK_HALF_WIDTH       5.0f

// Z-plane thresholds. Despawn compares the obstacle's *back edge*
// (z_world + half_d) against this value, not the centre, so a long
// cube (e.g. a 3-unit-long wall segment with half_d=1.5) doesn't
// vanish while the back edge is still poking up onto the screen.
// The threshold itself stays small enough that any active obstacle
// has already passed the ship at z=2.5.
#define WORLD_Z_NEAR_DESPAWN   0.6f
#define WORLD_Z_FAR_SPAWN      100.0f

// Default pixel-field cube dimensions and palette. Each obstacle
// carries its own dimensions and colours so the renderer doesn't
// need to know the difference between a small pixel cube, a wide
// gateway slab, a low wall segment, or a future pickup glyph.
#define OBSTACLE_HALF_W        0.4f
#define OBSTACLE_HEIGHT        2.0f
#define OBSTACLE_FRONT_COLOR   0xFFF71FF1u
#define OBSTACLE_SIDE_COLOR    0xFF7B1078u
#define OBSTACLE_TOP_COLOR     0xFFFDAFECu
#define OBSTACLE_OUTLINE_COLOR 0xFF31FBFBu

// Big-block area: 2× the pixel-field cube laterally and along z, same
// height, sparser cadence, grey palette so they read as solid masonry
// rather than pixels. Collide as CUBE-kind — physics and dispatch are
// identical to pixel-field cubes; only dimensions and colours differ.
#define BIG_HALF_W             (OBSTACLE_HALF_W * 2.0f)
#define BIG_HALF_D             (OBSTACLE_HALF_W * 2.0f)
#define BIG_HEIGHT             OBSTACLE_HEIGHT
#define BIG_FRONT_COLOR        0xFF808080u
#define BIG_SIDE_COLOR         0xFF505050u
#define BIG_TOP_COLOR          0xFFA0A0A0u
#define BIG_OUTLINE_COLOR      0xFF31FBFBu

// Gateway slabs: amber palette to distinguish them from both the
// magenta pixel field and the grey big blocks. Each gate is rendered
// as two CUBE-kind slabs flanking a central gap; head-on into either
// slab is fatal, scraping along the slab's z-trailing edge degrades
// speed exactly like brushing a pixel cube.
#define GATE_HALF_D            0.4f
#define GATE_HEIGHT            OBSTACLE_HEIGHT
#define GATE_FRONT_COLOR       0xFFE0A040u
#define GATE_SIDE_COLOR        0xFF905020u
#define GATE_TOP_COLOR         0xFFFFC880u
#define GATE_OUTLINE_COLOR     0xFF31FBFBu

// Side walls. One continuous wall on each side of the track, made of
// fixed-length cube segments so any future collision query hits a
// real obstacle entry. The segment length matches the floor's
// horizontal-stripe stride (FLOOR_LANE_L * FLOOR_HSTRIPE_DRAW_EVERY
// = 1 * 3) so each segment runs from one drawn grid line to the
// next. Inner face sits exactly at the track boundary
// (TRACK_HALF_WIDTH); outer face is WALL_HALF_W further out.
#define WALL_SEGMENT_LEN       3.0f
#define WALL_SEGMENT_HALF_D    (WALL_SEGMENT_LEN * 0.5f)
#define WALL_HALF_W            0.5f
#define WALL_HEIGHT            (OBSTACLE_HEIGHT / 3.0f)
#define WALL_X_RIGHT           (TRACK_HALF_WIDTH + WALL_HALF_W)
#define WALL_X_LEFT            (-(TRACK_HALF_WIDTH + WALL_HALF_W))
#define WALL_FRONT_COLOR       0xFF8C1A8Cu
#define WALL_SIDE_COLOR        0xFF551154u
#define WALL_TOP_COLOR         0xFFD040C5u
#define WALL_OUTLINE_COLOR     0xFF31FBFBu

// Area length bounds, world-z units. One "screen" is approximately
// the visible track depth (FLOOR_Z_FAR - FLOOR_Z_NEAR ≈ 58 u); the
// pixel-field and big-block areas run at least two screens so they
// feel like a coherent zone rather than a sprinkle of obstacles.
#define AREA_PIXEL_MIN_LEN     116.0f
#define AREA_PIXEL_MAX_LEN     232.0f
#define AREA_BIG_MIN_LEN       116.0f
#define AREA_BIG_MAX_LEN       232.0f

// Pixel-field spawn cadence (base, at stage 1). Each subsequent
// stage shrinks the interval by 5% down to a 0.5× floor (reached at
// stage ~10), so later stages feel meaningfully denser without ever
// becoming impassable.
#define PIXEL_INTERVAL_MIN     12.0f
#define PIXEL_INTERVAL_MAX     22.0f

// Big-block spawn cadence. Sparser than pixel field — the cubes are
// 4× the cross-section and would wall off the track if packed at
// pixel-field density. Same per-stage scaling.
#define BIG_INTERVAL_MIN       20.0f
#define BIG_INTERVAL_MAX       35.0f

// Gateway tuning. Ship full collision width is 2 * SHIP_COLLISION_HALF_W
// = 0.56 u (kept in sync with game.h by re-deriving from a literal here
// to avoid an awkward include cycle for a single constant). Opening
// scales linearly from 3× ship width at stage 1 down to 1.5× at
// stage 10 and clamps from there — that's where the difficulty curve
// lives. Inter-gate / lead-in / trailing pad is a fixed 50 u for all
// stages, sized so even back-to-back hard-left → hard-right alignment
// shifts are physically reachable at cruise speed.
#define SHIP_FULL_WIDTH        (2.0f * 0.28f)
#define GATEWAY_OPENING_STAGE1 (3.0f * SHIP_FULL_WIDTH)
#define GATEWAY_OPENING_STAGE10 (1.5f * SHIP_FULL_WIDTH)
#define GATEWAY_PAD_Z          50.0f
#define GATEWAY_COUNT_MIN      1
#define GATEWAY_COUNT_MAX      5

// Settling distance at the start of a gateway area. The previous
// area can spawn its last obstacle right up to the area boundary,
// placing it at camera-relative z = WORLD_Z_FAR_SPAWN. Waiting one
// full far-plane distance of camera travel guarantees that obstacle
// has crossed the camera and despawned before the gateway's
// alignment pad starts ticking — so the whole gateway area, lead-in
// included, is free of drifting leftovers and the player only has
// the gate itself to focus on. Reads visually as a deliberate
// breath before the alignment puzzle.
#define GATEWAY_SETTLE_Z       WORLD_Z_FAR_SPAWN

static uint32_t xorshift32(uint32_t* s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x ? x : 1u;
    return *s;
}

// Uniform float in [0, 1).
static float frand(uint32_t* s) {
    return (float)xorshift32(s) / 4294967296.0f;
}

// Stage seed derivation. Mixes the run's level seed with the stage
// index so each stage's content is fully determined by (seed, stage)
// — replaying the same seed reproduces every stage identically.
// Multiplier is the golden-ratio-derived constant used elsewhere in
// the code for hash mixing.
static uint32_t mix_stage_seed(uint32_t level_seed, uint8_t stage) {
    uint32_t s = level_seed ^ ((uint32_t)stage * 0x9E3779B9u);
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s ? s : 1u;
}

// Linear interpolation from stage 1 to stage 10. Outside [1,10] the
// endpoint values are held — gateways stop tightening past stage 10
// (the playfield would become unfair otherwise).
static float lerp_by_stage(uint8_t stage, float at_one, float at_ten) {
    if (stage <= 1)  return at_one;
    if (stage >= 10) return at_ten;
    float const t = (float)(stage - 1) / 9.0f;
    return at_one + (at_ten - at_one) * t;
}

// Spawn-interval multiplier for stage-driven density falloff. Stage 1
// → 1.0, stage 10 → 0.5, clamped at the 0.5 floor beyond.
static float stage_interval_scale(uint8_t stage) {
    float s = 1.0f - 0.05f * (float)((int)stage - 1);
    if (s < 0.5f) s = 0.5f;
    if (s > 1.0f) s = 1.0f;
    return s;
}

// Find a free obstacle slot and populate it. Returns true on success.
// On a full pool we silently drop the spawn — the pool is sized so
// that shouldn't happen with the current cadence + walls.
static bool spawn_obstacle(world_state_t* w, obstacle_kind_t kind,
                           float x, float z, float half_w, float half_d, float height,
                           uint32_t front_color, uint32_t side_color, uint32_t top_color,
                           uint32_t outline_color) {
    for (int i = 0; i < WORLD_OBSTACLE_POOL_SIZE; i++) {
        if (w->obstacles[i].active) continue;
        w->obstacles[i].kind          = kind;
        w->obstacles[i].x_world       = x;
        w->obstacles[i].z_world       = z;
        w->obstacles[i].half_w        = half_w;
        w->obstacles[i].half_d        = half_d;
        w->obstacles[i].height        = height;
        w->obstacles[i].front_color   = front_color;
        w->obstacles[i].side_color    = side_color;
        w->obstacles[i].top_color     = top_color;
        w->obstacles[i].outline_color = outline_color;
        w->obstacles[i].active        = true;
        return true;
    }
    return false;
}

// Top up either side wall so it covers the visible z range with
// segments evenly spaced by WALL_SEGMENT_LEN. The far cursor tracks
// the camera-relative z where the *next* far-end segment should
// land; subtracting `dz_world` each frame slides it closer to the
// camera, and whenever it dips inside the spawn window we add one
// more far segment and bump the cursor by one segment length. The
// segment centres land on `... -1.5, 1.5, 4.5, 7.5, ...` (offset by
// half a segment so each one runs from grid-line k to grid-line
// k+1), which keeps the wall's joints aligned with the floor's
// drawn horizontal stripes.
static void top_up_wall(world_state_t* w, float* far_cursor, float wall_x) {
    while (*far_cursor < WORLD_Z_FAR_SPAWN) {
        spawn_obstacle(w, OBSTACLE_KIND_WALL,
                       wall_x, *far_cursor, WALL_HALF_W, WALL_SEGMENT_HALF_D, WALL_HEIGHT,
                       WALL_FRONT_COLOR, WALL_SIDE_COLOR, WALL_TOP_COLOR, WALL_OUTLINE_COLOR);
        *far_cursor += WALL_SEGMENT_LEN;
    }
}

// Spawn one pixel-field cube at the far plane, x drawn uniformly
// across the track.
static void spawn_pixel_cube(world_state_t* w) {
    float const x = (frand(&w->stage_prng) * 2.0f - 1.0f) * TRACK_HALF_WIDTH;
    spawn_obstacle(w, OBSTACLE_KIND_CUBE,
                   x, WORLD_Z_FAR_SPAWN,
                   OBSTACLE_HALF_W, OBSTACLE_HALF_W, OBSTACLE_HEIGHT,
                   OBSTACLE_FRONT_COLOR, OBSTACLE_SIDE_COLOR, OBSTACLE_TOP_COLOR,
                   OBSTACLE_OUTLINE_COLOR);
}

// Spawn one speed-booster pickup at the far plane, x drawn
// uniformly across the track (same range as cube/big-block spawns
// so they share the playfield). Kind is PICKUP_BOOST so
// game_collide routes the contact to the boost-state-machine
// rather than to head-on death.
static void spawn_booster(world_state_t* w) {
    float const x = (frand(&w->stage_prng) * 2.0f - 1.0f) * TRACK_HALF_WIDTH;
    spawn_obstacle(w, OBSTACLE_KIND_PICKUP_BOOST,
                   x, WORLD_Z_FAR_SPAWN,
                   GAME_BOOSTER_HALF_W, GAME_BOOSTER_HALF_W, GAME_BOOSTER_HEIGHT,
                   GAME_BOOSTER_FRONT_COLOR, GAME_BOOSTER_SIDE_COLOR,
                   /* top_color */ GAME_BOOSTER_FRONT_COLOR,
                   GAME_BOOSTER_OUTLINE_COLOR);
}

// Spawn one big-block cube at the far plane, x drawn uniformly
// across the track.
static void spawn_big_block(world_state_t* w) {
    float const x = (frand(&w->stage_prng) * 2.0f - 1.0f) * TRACK_HALF_WIDTH;
    spawn_obstacle(w, OBSTACLE_KIND_CUBE,
                   x, WORLD_Z_FAR_SPAWN,
                   BIG_HALF_W, BIG_HALF_D, BIG_HEIGHT,
                   BIG_FRONT_COLOR, BIG_SIDE_COLOR, BIG_TOP_COLOR, BIG_OUTLINE_COLOR);
}

// Spawn a booster pickup at an explicit (x, z) — used when the area
// generator knows the right position (e.g. inside a gate gap) rather
// than picking randomly.
static void spawn_booster_at(world_state_t* w, float x, float z) {
    spawn_obstacle(w, OBSTACLE_KIND_PICKUP_BOOST,
                   x, z,
                   GAME_BOOSTER_HALF_W, GAME_BOOSTER_HALF_W, GAME_BOOSTER_HEIGHT,
                   GAME_BOOSTER_FRONT_COLOR, GAME_BOOSTER_SIDE_COLOR,
                   /* top_color */ GAME_BOOSTER_FRONT_COLOR,
                   GAME_BOOSTER_OUTLINE_COLOR);
}

// Spawn a gateway: two cube slabs flanking a central gap of width
// `2 * half_gap`. The gap centre is picked so that both slabs are
// entirely inside the playfield (no slab pokes through the side
// wall). Each slab is OBSTACLE_KIND_CUBE so the ship dies head-on
// just like striking a pixel cube — gateways are a deadly barrier,
// not a scrape rail.
//
// If `with_booster` is true, also spawns a speed booster centred in
// the gap at the same z as the gate — the gate area's way of
// consuming a stage-scheduled booster, placing it where the player
// must steer to dodge the gate anyway.
static void spawn_gateway(world_state_t* w, float half_gap, bool with_booster) {
    float const gap_centre_extent = TRACK_HALF_WIDTH - half_gap;
    float       gap_x             = 0.0f;
    if (gap_centre_extent > 0.0f) {
        gap_x = (frand(&w->stage_prng) * 2.0f - 1.0f) * gap_centre_extent;
    }
    float const left_inner   = gap_x - half_gap;
    float const left_outer   = -TRACK_HALF_WIDTH;
    float const right_inner  = gap_x + half_gap;
    float const right_outer  = TRACK_HALF_WIDTH;
    float const left_half_w  = (left_inner - left_outer) * 0.5f;
    float const left_centre  = (left_inner + left_outer) * 0.5f;
    float const right_half_w = (right_outer - right_inner) * 0.5f;
    float const right_centre = (right_outer + right_inner) * 0.5f;
    if (left_half_w > 0.0f) {
        spawn_obstacle(w, OBSTACLE_KIND_CUBE,
                       left_centre, WORLD_Z_FAR_SPAWN,
                       left_half_w, GATE_HALF_D, GATE_HEIGHT,
                       GATE_FRONT_COLOR, GATE_SIDE_COLOR, GATE_TOP_COLOR, GATE_OUTLINE_COLOR);
    }
    if (right_half_w > 0.0f) {
        spawn_obstacle(w, OBSTACLE_KIND_CUBE,
                       right_centre, WORLD_Z_FAR_SPAWN,
                       right_half_w, GATE_HALF_D, GATE_HEIGHT,
                       GATE_FRONT_COLOR, GATE_SIDE_COLOR, GATE_TOP_COLOR, GATE_OUTLINE_COLOR);
    }
    if (with_booster) {
        spawn_booster_at(w, gap_x, WORLD_Z_FAR_SPAWN);
    }
}

// Area initializers. Each picks the area's length budget and any
// type-specific state, then sets `next_event_z` so the first spawn
// fires after the appropriate lead-in distance.

static void area_init_pixel_field(area_state_t* a, uint8_t stage, uint32_t* prng) {
    float const scale    = stage_interval_scale(stage);
    float const interval = (PIXEL_INTERVAL_MIN
                           + frand(prng) * (PIXEL_INTERVAL_MAX - PIXEL_INTERVAL_MIN))
                          * scale;
    a->type               = AREA_TYPE_PIXEL_FIELD;
    a->length_remaining_z = AREA_PIXEL_MIN_LEN
                          + frand(prng) * (AREA_PIXEL_MAX_LEN - AREA_PIXEL_MIN_LEN);
    a->next_event_z       = interval;
    a->gates_remaining    = 0;
    a->gate_gap_half_w    = 0.0f;
    a->gate_pad_z         = 0.0f;
}

static void area_init_big_blocks(area_state_t* a, uint8_t stage, uint32_t* prng) {
    float const scale    = stage_interval_scale(stage);
    float const interval = (BIG_INTERVAL_MIN
                           + frand(prng) * (BIG_INTERVAL_MAX - BIG_INTERVAL_MIN))
                          * scale;
    a->type               = AREA_TYPE_BIG_BLOCKS;
    a->length_remaining_z = AREA_BIG_MIN_LEN
                          + frand(prng) * (AREA_BIG_MAX_LEN - AREA_BIG_MIN_LEN);
    a->next_event_z       = interval;
    a->gates_remaining    = 0;
    a->gate_gap_half_w    = 0.0f;
    a->gate_pad_z         = 0.0f;
}

static void area_init_gateways(area_state_t* a, uint8_t stage, uint32_t* prng) {
    int n = GATEWAY_COUNT_MIN
          + (int)(frand(prng) * (float)(GATEWAY_COUNT_MAX - GATEWAY_COUNT_MIN + 1));
    if (n > GATEWAY_COUNT_MAX) n = GATEWAY_COUNT_MAX;
    float const gap     = lerp_by_stage(stage, GATEWAY_OPENING_STAGE1, GATEWAY_OPENING_STAGE10);
    float const pad     = GATEWAY_PAD_Z;
    float const thick   = 2.0f * GATE_HALF_D;
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

static void area_init_rest(area_state_t* a) {
    a->type               = AREA_TYPE_REST;
    a->length_remaining_z = WORLD_REST_LENGTH_Z;
    a->next_event_z       = WORLD_REST_LENGTH_Z;  // nothing to spawn here today
    a->gates_remaining    = 0;
    a->gate_gap_half_w    = 0.0f;
    a->gate_pad_z         = 0.0f;
}

// Pick a random applicable area type for this stage. All three
// obstacle area types unlock at stage 1, so today the picker is a
// uniform draw over {PIXEL_FIELD, GATEWAYS, BIG_BLOCKS}. New area
// types add an entry here gated by their min_stage.
static area_type_t pick_area_type(uint8_t stage, uint32_t* prng) {
    area_type_t candidates[3];
    int         n = 0;
    candidates[n++] = AREA_TYPE_PIXEL_FIELD;  // min stage 1
    candidates[n++] = AREA_TYPE_GATEWAYS;     // min stage 1
    candidates[n++] = AREA_TYPE_BIG_BLOCKS;   // min stage 1
    (void)stage;
    uint32_t const r = xorshift32(prng);
    return candidates[r % (uint32_t)n];
}

static void start_next_area(world_state_t* w) {
    area_type_t const t = pick_area_type(w->stage, &w->stage_prng);
    switch (t) {
        case AREA_TYPE_PIXEL_FIELD: area_init_pixel_field(&w->area, w->stage, &w->stage_prng); break;
        case AREA_TYPE_GATEWAYS:    area_init_gateways(&w->area,    w->stage, &w->stage_prng); break;
        case AREA_TYPE_BIG_BLOCKS:  area_init_big_blocks(&w->area,  w->stage, &w->stage_prng); break;
        case AREA_TYPE_REST:        area_init_rest(&w->area);                                  break;
    }
}

static void start_stage(world_state_t* w, uint8_t stage) {
    w->stage             = stage;
    w->stage_z_remaining = WORLD_STAGE_LENGTH_Z;
    w->stage_prng        = mix_stage_seed(w->level_seed, stage);
    // Clear any unspent owed-counter from the previous stage. The
    // rest-entry handler should already have drained it, but reset
    // here defensively so a logic bug doesn't accumulate across runs.
    w->area.boosters_owed = 0;

    // Schedule the stage's boosters: divide the stage length into
    // N equal segments and place one booster in each segment at a
    // jittered position (0.25..0.75 of the segment). Roughly equal
    // spacing, deterministic from the stage seed.
    float const segment = WORLD_STAGE_LENGTH_Z / (float)GAME_BOOSTERS_PER_STAGE;
    for (int i = 0; i < GAME_BOOSTERS_PER_STAGE; i++) {
        float const jitter = 0.25f + 0.5f * frand(&w->stage_prng);
        w->booster_due_at_progress[i] = ((float)i + jitter) * segment;
    }

    start_next_area(w);
}

// Tick the active area by `dz` world-z units. Spawns any due
// obstacle events; returns true when the area's length budget is
// exhausted (caller decides what comes next).
static bool area_tick(world_state_t* w, float dz) {
    area_state_t* a = &w->area;
    a->length_remaining_z -= dz;
    a->next_event_z       -= dz;

    switch (a->type) {
        case AREA_TYPE_PIXEL_FIELD: {
            // When the stage scheduler has flagged a booster as owed,
            // the next spawn event becomes a booster instead of a
            // cube. This guarantees no overlap with adjacent obstacles
            // — the booster simply occupies a slot that would have
            // held a cube. One cube is "displaced" per booster, which
            // is acceptable density-wise.
            float const scale = stage_interval_scale(w->stage);
            while (a->next_event_z <= 0.0f && a->length_remaining_z > 0.0f) {
                if (a->boosters_owed > 0) {
                    spawn_booster(w);
                    a->boosters_owed--;
                } else {
                    spawn_pixel_cube(w);
                }
                float const interval = (PIXEL_INTERVAL_MIN
                                       + frand(&w->stage_prng)
                                         * (PIXEL_INTERVAL_MAX - PIXEL_INTERVAL_MIN))
                                      * scale;
                a->next_event_z += interval;
            }
            break;
        }
        case AREA_TYPE_BIG_BLOCKS: {
            // Same booster-displaces-cube rule as pixel field.
            float const scale = stage_interval_scale(w->stage);
            while (a->next_event_z <= 0.0f && a->length_remaining_z > 0.0f) {
                if (a->boosters_owed > 0) {
                    spawn_booster(w);
                    a->boosters_owed--;
                } else {
                    spawn_big_block(w);
                }
                float const interval = (BIG_INTERVAL_MIN
                                       + frand(&w->stage_prng)
                                         * (BIG_INTERVAL_MAX - BIG_INTERVAL_MIN))
                                      * scale;
                a->next_event_z += interval;
            }
            break;
        }
        case AREA_TYPE_GATEWAYS: {
            // Each event spawns one gate; the empty pad between gates
            // is just the camera travelling `pad + 2*half_d` (one pad
            // plus the next gate's thickness) before the next event
            // fires. After the last gate the trailing pad ticks down
            // the area's length normally.
            //
            // If a booster is owed, it rides along inside the next
            // gate's gap — the player has to steer to the gap anyway
            // to dodge the gate, so picking up the booster is the
            // default path. Reads as the area type rewarding tight
            // alignment with a speed boost.
            float const thick = 2.0f * GATE_HALF_D;
            while (a->next_event_z <= 0.0f && a->gates_remaining > 0) {
                bool const with_booster = a->boosters_owed > 0;
                spawn_gateway(w, a->gate_gap_half_w, with_booster);
                if (with_booster) a->boosters_owed--;
                a->gates_remaining--;
                a->next_event_z += a->gate_pad_z + thick;
            }
            break;
        }
        case AREA_TYPE_REST:
            // No tick-driven spawns here; boosters owed at rest entry
            // are dumped all at once in `world_advance` when the rest
            // area is inserted (see the area-transition block).
            break;
    }

    return a->length_remaining_z <= 0.0f;
}

void world_init(world_state_t* w, uint32_t seed) {
    for (int i = 0; i < WORLD_OBSTACLE_POOL_SIZE; i++) {
        w->obstacles[i].active = false;
    }
    w->level_seed       = seed ? seed : 1u;
    // Start each wall's far cursor at half a segment so the first
    // segment is centred at z = WALL_SEGMENT_HALF_D (= 1.5), running
    // from z=0 to z=3 — between the first two drawn grid stripes.
    w->right_wall_far_z = WALL_SEGMENT_HALF_D;
    w->left_wall_far_z  = WALL_SEGMENT_HALF_D;
    top_up_wall(w, &w->right_wall_far_z, WALL_X_RIGHT);
    top_up_wall(w, &w->left_wall_far_z,  WALL_X_LEFT);
    // Stage 1 starts immediately. The first area's lead-in (pixel/big
    // interval, or the gateway's lead-in pad) plus the far-plane
    // spawn distance gives the player ~5 s of clear track before the
    // first obstacle reaches them — that's our "starting rest" the
    // user described, without needing an explicit rest area up front.
    start_stage(w, 1);
}

void world_advance(world_state_t* w, float dt, float speed_z) {
    float const dz = speed_z * dt;

    // Move active obstacles toward the camera and despawn those that
    // have fully passed the near plane.
    for (int i = 0; i < WORLD_OBSTACLE_POOL_SIZE; i++) {
        if (!w->obstacles[i].active) continue;
        w->obstacles[i].z_world -= dz;
        if (w->obstacles[i].z_world + w->obstacles[i].half_d < WORLD_Z_NEAR_DESPAWN) {
            w->obstacles[i].active = false;
        }
    }

    // Slide the wall cursors with the camera and refill anything
    // that's drifted into the spawn window. Done after the despawn
    // pass so freed slots are immediately available.
    w->right_wall_far_z -= dz;
    w->left_wall_far_z  -= dz;
    top_up_wall(w, &w->right_wall_far_z, WALL_X_RIGHT);
    top_up_wall(w, &w->left_wall_far_z,  WALL_X_LEFT);

    // Drive the stage / area state machine. The active area emits
    // its scheduled obstacles; when it finishes, we either pick the
    // next area in this stage, insert the rest area (if the stage
    // budget is exhausted), or advance to the next stage (if the
    // rest area just finished).
    w->stage_z_remaining -= dz;
    bool const area_done = area_tick(w, dz);

    // Booster scheduler — decides *when* a booster is due. Each
    // scheduled progress mark, once crossed, bumps the active area's
    // `boosters_owed` counter; the area's tick block (above) consumes
    // it and decides *where* the booster goes. Spent slots are
    // marked with a negative sentinel so they don't fire again. Rest
    // areas skip this loop — their booster handling is the dump on
    // entry, just below.
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
            // Capture any leftovers before area_init_rest overwrites
            // most of the area state (it doesn't touch boosters_owed,
            // but read it explicitly so the intent is local).
            int const leftover_boosters = w->area.boosters_owed;
            area_init_rest(&w->area);
            // Rest absorbs the per-rest quota plus anything the stage
            // generators couldn't place. All spawn at the rest's far
            // plane with random x, in a single burst.
            int const total = leftover_boosters + GAME_BOOSTERS_PER_REST;
            for (int i = 0; i < total; i++) {
                spawn_booster(w);
            }
            w->area.boosters_owed = 0;
        } else {
            start_next_area(w);
        }
    }
}
