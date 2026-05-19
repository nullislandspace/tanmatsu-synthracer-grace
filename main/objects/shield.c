#include "objects/shield.h"

#include <math.h>
#include <stdint.h>

#include "esp_timer.h"
#include "magicnumbers.h"
#include "render.h"      // RENDER_NEAR_CLIP_Z
#include "scene.h"       // scene_tri, scene_line
#include "world.h"

// Violet palette — distinct from the blue Tri, green speed booster
// and red jump booster. Caps are the bright violet, the rim the
// darker shade, the wireframe pale violet.
#define SHIELD_FRONT_COLOR    0xFFB060FFu   // hexagon caps
#define SHIELD_SIDE_COLOR     0xFF6028A0u   // extruded rim
#define SHIELD_OUTLINE_COLOR  0xFFD8B0FFu   // wireframe

// Plate thickness as a fraction of the hexagon radius — thin, so it
// reads as a plate / medallion rather than a solid prism.
#define SHIELD_THICKNESS_FRAC 0.32f

// Unit pointy-top hexagon corners in the local (u = horizontal,
// v = up) plane — corner k at angle 90 + 60*k degrees.
static float const HEX[6][2] = {
    {  0.000000f,  1.000000f },
    { -0.866025f,  0.500000f },
    { -0.866025f, -0.500000f },
    {  0.000000f, -1.000000f },
    {  0.866025f, -0.500000f },
    {  0.866025f,  0.500000f },
};

// Emit callback — a violet hexagonal plate spinning around Y, one
// full turn per GAME_BOOSTER_ROTATION_PERIOD_S (shared cadence with
// the other pickups). Face-on it reads as a hexagonal medallion;
// edge-on as a thin sliver. The depth buffer resolves occlusion, so
// every face is emitted unconditionally.
static void shield_emit(obstacle_t const* o) {
    if (o->z_world < RENDER_NEAR_CLIP_Z) return;

    int64_t const now_us = esp_timer_get_time();
    float   const time_s = (float)(now_us % 600000000LL) * 1e-6f;
    float   const angle  = fmodf(time_s, GAME_BOOSTER_ROTATION_PERIOD_S)
                         / GAME_BOOSTER_ROTATION_PERIOD_S * (2.0f * (float)M_PI);
    float const cosa = cosf(angle);
    float const sina = sinf(angle);

    float const R        = o->half_w;
    float const half_t   = R * SHIELD_THICKNESS_FRAC * 0.5f;
    float const y_centre = o->y_base + o->height * 0.5f;

    // 12 world verts: a front ring at local z = +half_t and a back
    // ring at -half_t, each Y-rotated by the spin angle. Y-rotation:
    // (x,y,z) -> (x cos + z sin, y, -x sin + z cos).
    float fx[6], fy[6], fz[6];   // front cap
    float bx[6], by[6], bz[6];   // back cap
    for (int i = 0; i < 6; i++) {
        float const u = HEX[i][0] * R;
        float const v = HEX[i][1] * R;
        fx[i] = o->x_world + u * cosa + half_t * sina;
        fy[i] = y_centre   + v;
        fz[i] = o->z_world - u * sina + half_t * cosa;
        bx[i] = o->x_world + u * cosa - half_t * sina;
        by[i] = y_centre   + v;
        bz[i] = o->z_world - u * sina - half_t * cosa;
    }

    uint32_t const cap = o->front_color;
    uint32_t const rim = o->side_color;
    uint32_t const out = o->outline_color;

    // Front + back hexagon caps — triangle fan from corner 0.
    for (int i = 1; i < 5; i++) {
        int const j = i + 1;
        scene_tri(fx[0], fy[0], fz[0], fx[i], fy[i], fz[i], fx[j], fy[j], fz[j], cap);
        scene_tri(bx[0], by[0], bz[0], bx[i], by[i], bz[i], bx[j], by[j], bz[j], cap);
    }
    // Rim — one quad (two triangles) per hexagon edge.
    for (int i = 0; i < 6; i++) {
        int const j = (i + 1) % 6;
        scene_tri(fx[i], fy[i], fz[i], fx[j], fy[j], fz[j], bx[j], by[j], bz[j], rim);
        scene_tri(fx[i], fy[i], fz[i], bx[j], by[j], bz[j], bx[i], by[i], bz[i], rim);
    }
    // Wireframe — front hexagon, back hexagon, and the 6 connectors.
    for (int i = 0; i < 6; i++) {
        int const j = (i + 1) % 6;
        scene_line(fx[i], fy[i], fz[i], fx[j], fy[j], fz[j], out);
        scene_line(bx[i], by[i], bz[i], bx[j], by[j], bz[j], out);
        scene_line(fx[i], fy[i], fz[i], bx[i], by[i], bz[i], out);
    }
}

obstacle_t* shield_spawn_at(world_state_t* w, float x, float z) {
    obstacle_t* const o = obstacle_spawn(
        w, OBSTACLE_KIND_PICKUP_SHIELD,
        x, z,
        GAME_BOOSTER_HALF_W, GAME_BOOSTER_HALF_W, GAME_BOOSTER_HEIGHT,
        SHIELD_FRONT_COLOR, SHIELD_SIDE_COLOR,
        /* top_color (unused — custom emit) */ SHIELD_FRONT_COLOR,
        SHIELD_OUTLINE_COLOR);
    if (o) o->emit = shield_emit;
    return o;
}

obstacle_t* shield_spawn(world_state_t* w) {
    // Same wall-clearance rule as the speed / jump boosters: the
    // outer face of the footprint stays inside the playfield.
    float const x_extent = TRACK_HALF_WIDTH - GAME_BOOSTER_HALF_W;
    float const x        = (world_frand(&w->stage_prng) * 2.0f - 1.0f) * x_extent;
    return shield_spawn_at(w, x, WORLD_Z_FAR_SPAWN);
}
