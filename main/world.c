#include "world.h"

// Track geometry. The track is `2 * TRACK_HALF_WIDTH` world units wide,
// centred on x=0. The playfield is intentionally much wider than the
// viewport at ship-z (~±2.3 visible units) — the camera pans laterally
// with the ship so the player can move sideways to find clear lanes,
// matching Race The Sun's behavior.
#define TRACK_HALF_WIDTH       5.0f

// Z-plane thresholds. The near-plane despawn must be smaller than the
// ship's z-position so an obstacle can pass beneath/through the ship
// before being recycled.
#define WORLD_Z_NEAR_DESPAWN   0.6f
#define WORLD_Z_FAR_SPAWN      100.0f

// Spawn cadence in world-z units. With BASE_SPEED = 12 u/s this gives
// roughly 0.5-1 obstacle per second at the camera, sparse enough to
// see clear lanes and react in time. Phase 10's regional system will
// override these via per-region mutators.
#define SPAWN_INTERVAL_MIN_Z   12.0f
#define SPAWN_INTERVAL_MAX_Z   22.0f

// Default obstacle dimensions. Phase 3 uses one shape; Phase 9 will
// expand with multiple kinds.
#define OBSTACLE_HALF_W        0.4f
#define OBSTACLE_HEIGHT        2.0f

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

void world_init(world_state_t* w, uint32_t seed) {
    for (int i = 0; i < WORLD_OBSTACLE_POOL_SIZE; i++) {
        w->obstacles[i].active = false;
    }
    w->next_spawn_z = SPAWN_INTERVAL_MIN_Z;
    w->prng_state  = seed ? seed : 1u;
}

static void try_spawn(world_state_t* w) {
    for (int i = 0; i < WORLD_OBSTACLE_POOL_SIZE; i++) {
        if (w->obstacles[i].active) continue;
        w->obstacles[i].x_world = (frand(&w->prng_state) * 2.0f - 1.0f) * TRACK_HALF_WIDTH;
        w->obstacles[i].z_world = WORLD_Z_FAR_SPAWN;
        w->obstacles[i].half_w  = OBSTACLE_HALF_W;
        w->obstacles[i].height  = OBSTACLE_HEIGHT;
        w->obstacles[i].active  = true;
        return;
    }
    // Pool full — drop the spawn. With 64 slots and the spawn cadence
    // above we should never hit this in practice.
}

void world_advance(world_state_t* w, float dt, float speed_z) {
    float const dz = speed_z * dt;

    for (int i = 0; i < WORLD_OBSTACLE_POOL_SIZE; i++) {
        if (!w->obstacles[i].active) continue;
        w->obstacles[i].z_world -= dz;
        if (w->obstacles[i].z_world < WORLD_Z_NEAR_DESPAWN) {
            w->obstacles[i].active = false;
        }
    }

    w->next_spawn_z -= dz;
    while (w->next_spawn_z <= 0.0f) {
        try_spawn(w);
        float const r = frand(&w->prng_state);
        w->next_spawn_z += SPAWN_INTERVAL_MIN_Z + r * (SPAWN_INTERVAL_MAX_Z - SPAWN_INTERVAL_MIN_Z);
    }
}
