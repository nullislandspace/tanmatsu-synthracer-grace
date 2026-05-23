#include "objects/checkpoint.h"

#include <math.h>
#include <stdint.h>

#include "esp_timer.h"
#include "magicnumbers.h"
#include "render.h"      // render_camera, RENDER_NEAR_CLIP_Z
#include "se_scene.h"       // scene_tri, scene_line
#include "world.h"

// Soccer-ball palette — a black-and-white reskin of the speed
// booster's icosahedron. Per-face lighting still shades the faces so
// the ball reads as a 3D faceted gem rather than a flat silhouette.
#define CHECKPOINT_BLACK_COLOR    0xFF181818u
#define CHECKPOINT_WHITE_COLOR    0xFFE8E8E8u
#define CHECKPOINT_OUTLINE_COLOR  0xFF808080u

// Golden ratio + the regular-icosahedron vertex / face tables —
// identical geometry to the speed booster (render.c's emit path).
#define ICO_PHI 1.618033988749895f
#define ICO_SCALE (GAME_BOOSTER_HALF_W / ICO_PHI)

static float const ICO_VERTS[12][3] = {
    { 0.0f,     1.0f,     ICO_PHI },
    { 0.0f,     1.0f,    -ICO_PHI },
    { 0.0f,    -1.0f,     ICO_PHI },
    { 0.0f,    -1.0f,    -ICO_PHI },
    { 1.0f,     ICO_PHI,  0.0f    },
    { 1.0f,    -ICO_PHI,  0.0f    },
    {-1.0f,     ICO_PHI,  0.0f    },
    {-1.0f,    -ICO_PHI,  0.0f    },
    { ICO_PHI,  0.0f,     1.0f    },
    { ICO_PHI,  0.0f,    -1.0f    },
    {-ICO_PHI,  0.0f,     1.0f    },
    {-ICO_PHI,  0.0f,    -1.0f    },
};

static uint8_t const ICO_FACES[20][3] = {
    {  0,  8,  4 }, {  0,  4,  6 }, {  0,  6, 10 }, {  0, 10,  2 }, {  0,  2,  8 },
    {  3,  1,  9 }, {  3, 11,  1 }, {  3,  7, 11 }, {  3,  5,  7 }, {  3,  9,  5 },
    {  4,  8,  9 }, {  1,  4,  9 }, {  6,  4,  1 }, {  1, 11,  6 }, {  6, 11, 10 },
    { 10, 11,  7 }, { 10,  7,  2 }, {  2,  7,  5 }, {  2,  5,  8 }, {  8,  5,  9 },
};

// Per-face black/white assignment — alternating within each vertex
// cap and around the equatorial band so the ball reads as a checkered
// soccer ball rather than two solid hemispheres.
static uint8_t const FACE_BLACK[20] = {
    1, 0, 1, 0, 1,   1, 0, 1, 0, 1,
    1, 0, 1, 0, 1,   0, 1, 0, 1, 0,
};

// Scale every RGB channel of an ARGB colour by `s` (0..1); alpha
// kept. Used for the per-face lighting tint.
static inline uint32_t cp_dim(uint32_t col, float s) {
    uint32_t const a = (col >> 24) & 0xFFu;
    uint32_t const r = (uint32_t)((float)((col >> 16) & 0xFFu) * s);
    uint32_t const g = (uint32_t)((float)((col >>  8) & 0xFFu) * s);
    uint32_t const b = (uint32_t)((float)((col >>  0) & 0xFFu) * s);
    return (a << 24) | (r << 16) | (g << 8) | b;
}

// Emit callback — a black-and-white icosahedron spinning around Y on
// the shared booster cadence. Same pipeline as the speed booster:
// rotate the 12 verts, back-face cull each face by its centroid
// direction, light it, emit. Only the face colours differ.
static void checkpoint_emit(obstacle_t const* o) {
    if (o->z_world < RENDER_NEAR_CLIP_Z) return;

    int64_t const now_us = esp_timer_get_time();
    float   const time_s = (float)(now_us % 600000000LL) * 1e-6f;
    float   const angle  = fmodf(time_s, GAME_BOOSTER_ROTATION_PERIOD_S)
                         / GAME_BOOSTER_ROTATION_PERIOD_S * (2.0f * (float)M_PI);
    float const cosa = cosf(angle);
    float const sina = sinf(angle);

    float const y_centre = o->y_base + GAME_BOOSTER_HEIGHT * 0.5f;

    float lvx[12], lvy[12], lvz[12];
    float wvx[12], wvy[12], wvz[12];
    for (int i = 0; i < 12; i++) {
        float const x = ICO_VERTS[i][0] * ICO_SCALE;
        float const y = ICO_VERTS[i][1] * ICO_SCALE;
        float const z = ICO_VERTS[i][2] * ICO_SCALE;
        float const xr =  x * cosa + z * sina;
        float const yr =  y;
        float const zr = -x * sina + z * cosa;
        lvx[i] = xr; lvy[i] = yr; lvz[i] = zr;
        wvx[i] = o->x_world + xr;
        wvy[i] = y_centre   + yr;
        wvz[i] = o->z_world + zr;
    }

    // Lighting direction (front-top-left, fixed in world space).
    float const light_x = -0.4f;
    float const light_y =  0.7f;
    float const light_z = -0.6f;

    render_camera_t const cam = render_camera();

    for (int f = 0; f < 20; f++) {
        int const a = ICO_FACES[f][0];
        int const b = ICO_FACES[f][1];
        int const c = ICO_FACES[f][2];

        float const cx_l = (lvx[a] + lvx[b] + lvx[c]) * (1.0f / 3.0f);
        float const cy_l = (lvy[a] + lvy[b] + lvy[c]) * (1.0f / 3.0f);
        float const cz_l = (lvz[a] + lvz[b] + lvz[c]) * (1.0f / 3.0f);

        float const cx_w = o->x_world + cx_l;
        float const cy_w = y_centre   + cy_l;
        float const cz_w = o->z_world + cz_l;
        float const dvx  = cam.x - cx_w;
        float const dvy  = cam.y - cy_w;
        float const dvz  = 0.0f  - cz_w;

        // Back-face cull.
        if (cx_l * dvx + cy_l * dvy + cz_l * dvz <= 0.0f) continue;

        // Lighting tint from the normalised face normal.
        float const len     = sqrtf(cx_l * cx_l + cy_l * cy_l + cz_l * cz_l);
        float const inv_len = (len > 1e-6f) ? (1.0f / len) : 0.0f;
        float const nx = cx_l * inv_len;
        float const ny = cy_l * inv_len;
        float const nz = cz_l * inv_len;
        float       d  = nx * light_x + ny * light_y + nz * light_z;
        if (d < 0.0f) d = 0.0f;
        float const tint = 0.55f + 0.45f * d;

        uint32_t const base = FACE_BLACK[f] ? CHECKPOINT_BLACK_COLOR
                                            : CHECKPOINT_WHITE_COLOR;

        scene_tri(wvx[a], wvy[a], wvz[a],
                  wvx[b], wvy[b], wvz[b],
                  wvx[c], wvy[c], wvz[c],
                  cp_dim(base, tint));
        scene_line(wvx[a], wvy[a], wvz[a], wvx[b], wvy[b], wvz[b], CHECKPOINT_OUTLINE_COLOR);
        scene_line(wvx[b], wvy[b], wvz[b], wvx[c], wvy[c], wvz[c], CHECKPOINT_OUTLINE_COLOR);
        scene_line(wvx[c], wvy[c], wvz[c], wvx[a], wvy[a], wvz[a], CHECKPOINT_OUTLINE_COLOR);
    }
}

obstacle_t* checkpoint_spawn_at(world_state_t* w, float x, float z) {
    obstacle_t* const o = obstacle_spawn(
        w, OBSTACLE_KIND_PICKUP_CHECKPOINT,
        x, z,
        GAME_BOOSTER_HALF_W, GAME_BOOSTER_HALF_W, GAME_BOOSTER_HEIGHT,
        CHECKPOINT_WHITE_COLOR, CHECKPOINT_BLACK_COLOR,
        /* top_color (unused — custom emit) */ CHECKPOINT_WHITE_COLOR,
        CHECKPOINT_OUTLINE_COLOR);
    if (o) o->emit = checkpoint_emit;
    return o;
}

obstacle_t* checkpoint_spawn(world_state_t* w) {
    float const x_extent = TRACK_HALF_WIDTH - GAME_BOOSTER_HALF_W;
    float const x        = (world_frand(&w->stage_prng) * 2.0f - 1.0f) * x_extent;
    return checkpoint_spawn_at(w, x, WORLD_Z_FAR_SPAWN);
}
