#include "objects/cube.h"

#include "world.h"

// --- Palettes -------------------------------------------------------
// Per-variant colour sets. Kept here so retuning a single object's
// look doesn't require editing world.c or chasing palette literals
// across the codebase.

// Pixel field — magenta cubes (default obstacle look).
#define PIXEL_FRONT_COLOR   0xFFF71FF1u
#define PIXEL_SIDE_COLOR    0xFF7B1078u
#define PIXEL_TOP_COLOR     0xFFFDAFECu
#define PIXEL_OUTLINE_COLOR 0xFF31FBFBu

// Big blocks — grey masonry palette so they read as solid rather
// than pixels.
#define BIG_FRONT_COLOR     0xFF808080u
#define BIG_SIDE_COLOR      0xFF505050u
#define BIG_TOP_COLOR       0xFFA0A0A0u
#define BIG_OUTLINE_COLOR   0xFF31FBFBu

// Gateway slabs — amber so they're visually distinct from pixel
// field / big blocks.
#define GATE_FRONT_COLOR    0xFFE0A040u
#define GATE_SIDE_COLOR     0xFF905020u
#define GATE_TOP_COLOR      0xFFFFC880u
#define GATE_OUTLINE_COLOR  0xFF31FBFBu

obstacle_t* cube_spawn_pixel(world_state_t* w) {
    float const x = (world_frand(&w->stage_prng) * 2.0f - 1.0f) * TRACK_HALF_WIDTH;
    return obstacle_spawn(w, OBSTACLE_KIND_CUBE,
                          x, WORLD_Z_FAR_SPAWN,
                          CUBE_PIXEL_HALF_W, CUBE_PIXEL_HALF_W, CUBE_PIXEL_HEIGHT,
                          PIXEL_FRONT_COLOR, PIXEL_SIDE_COLOR,
                          PIXEL_TOP_COLOR,  PIXEL_OUTLINE_COLOR);
}

obstacle_t* cube_spawn_big(world_state_t* w) {
    float const x = (world_frand(&w->stage_prng) * 2.0f - 1.0f) * TRACK_HALF_WIDTH;
    return obstacle_spawn(w, OBSTACLE_KIND_CUBE,
                          x, WORLD_Z_FAR_SPAWN,
                          CUBE_BIG_HALF_W, CUBE_BIG_HALF_D, CUBE_BIG_HEIGHT,
                          BIG_FRONT_COLOR, BIG_SIDE_COLOR,
                          BIG_TOP_COLOR,  BIG_OUTLINE_COLOR);
}

obstacle_t* cube_spawn_gate_slab(world_state_t* w, float x, float z, float half_w) {
    if (half_w <= 0.0f) return NULL;
    return obstacle_spawn(w, OBSTACLE_KIND_CUBE,
                          x, z,
                          half_w, CUBE_GATE_HALF_D, CUBE_GATE_HEIGHT,
                          GATE_FRONT_COLOR, GATE_SIDE_COLOR,
                          GATE_TOP_COLOR,  GATE_OUTLINE_COLOR);
}
