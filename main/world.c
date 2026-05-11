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

// Spawn cadence for dynamic obstacles, in world-z units. With
// BASE_SPEED ≈ 12 u/s this gives roughly 0.5-1 obstacle per second
// at the camera, sparse enough to see clear lanes and react in time.
// Phase 10's regional system will override these via per-region
// mutators.
#define SPAWN_INTERVAL_MIN_Z   12.0f
#define SPAWN_INTERVAL_MAX_Z   22.0f

// Default dynamic-obstacle dimensions and palette. Each obstacle now
// carries its own dimensions and colours so the renderer doesn't
// need to know the difference between a tall pillar, a low wall
// segment, or a future pickup glyph.
#define OBSTACLE_HALF_W        0.4f
#define OBSTACLE_HEIGHT        2.0f
#define OBSTACLE_FRONT_COLOR   0xFFF71FF1u
#define OBSTACLE_SIDE_COLOR    0xFF7B1078u
#define OBSTACLE_TOP_COLOR     0xFFFDAFECu
#define OBSTACLE_OUTLINE_COLOR 0xFF31FBFBu

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

void world_init(world_state_t* w, uint32_t seed) {
    for (int i = 0; i < WORLD_OBSTACLE_POOL_SIZE; i++) {
        w->obstacles[i].active = false;
    }
    w->next_spawn_z      = SPAWN_INTERVAL_MIN_Z;
    w->prng_state        = seed ? seed : 1u;
    // Start each wall's far cursor at half a segment so the first
    // segment is centred at z = WALL_SEGMENT_HALF_D (= 1.5), running
    // from z=0 to z=3 — between the first two drawn grid stripes.
    w->right_wall_far_z  = WALL_SEGMENT_HALF_D;
    w->left_wall_far_z   = WALL_SEGMENT_HALF_D;
    top_up_wall(w, &w->right_wall_far_z, WALL_X_RIGHT);
    top_up_wall(w, &w->left_wall_far_z,  WALL_X_LEFT);
}

static void try_spawn_dynamic(world_state_t* w) {
    spawn_obstacle(w, OBSTACLE_KIND_CUBE,
                   (frand(&w->prng_state) * 2.0f - 1.0f) * TRACK_HALF_WIDTH,
                   WORLD_Z_FAR_SPAWN,
                   OBSTACLE_HALF_W, OBSTACLE_HALF_W, OBSTACLE_HEIGHT,
                   OBSTACLE_FRONT_COLOR, OBSTACLE_SIDE_COLOR, OBSTACLE_TOP_COLOR, OBSTACLE_OUTLINE_COLOR);
}

void world_advance(world_state_t* w, float dt, float speed_z) {
    float const dz = speed_z * dt;

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

    w->next_spawn_z -= dz;
    while (w->next_spawn_z <= 0.0f) {
        try_spawn_dynamic(w);
        float const r = frand(&w->prng_state);
        w->next_spawn_z += SPAWN_INTERVAL_MIN_Z + r * (SPAWN_INTERVAL_MAX_Z - SPAWN_INTERVAL_MIN_Z);
    }
}
