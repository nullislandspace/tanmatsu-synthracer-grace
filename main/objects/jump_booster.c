#include "objects/jump_booster.h"

#include <math.h>
#include <stdint.h>

#include "direct_565.h"
#include "esp_timer.h"
#include "magicnumbers.h"
#include "render.h"      // render_project, render_camera, RENDER_NEAR_CLIP_Z
#include "world.h"

// Red palette — distinct from the green speed booster and the
// blue Tri. front / side give the octahedron two-tone faceting;
// outline is the bright wireframe.
#define JUMP_FRONT_COLOR    0xFFFF4848u
#define JUMP_SIDE_COLOR     0xFFA02828u
#define JUMP_OUTLINE_COLOR  0xFFFF9090u

// Fixed light direction (front-top-left) for per-face shading, so
// the octahedron reads as a lit, rotating gem rather than showing a
// fixed bright/dark pattern.
#define JUMP_LIGHT_X   (-0.4f)
#define JUMP_LIGHT_Y   ( 0.7f)
#define JUMP_LIGHT_Z   (-0.6f)

// Scale every RGB channel of an ARGB colour by `s` (0..1); alpha
// kept. Used for the per-face lighting tint.
static inline uint32_t jump_dim(uint32_t col, float s) {
    uint32_t const a = (col >> 24) & 0xFFu;
    uint32_t const r = (uint32_t)((float)((col >> 16) & 0xFFu) * s);
    uint32_t const g = (uint32_t)((float)((col >>  8) & 0xFFu) * s);
    uint32_t const b = (uint32_t)((float)((col >>  0) & 0xFFu) * s);
    return (a << 24) | (r << 16) | (g << 8) | b;
}

// Octahedron — 6 vertices (±1 on each axis) and 8 triangular faces,
// each joining one vertex from each axis. Winding is irrelevant:
// back-face culling uses the face centroid direction (the outward
// normal of any face of a centred convex polyhedron), not a
// cross-product, so the cull is purely geometric.
//   vert index: 0 +x, 1 -x, 2 +y, 3 -y, 4 +z, 5 -z
static float const OCT_VERTS[6][3] = {
    {  1.0f,  0.0f,  0.0f }, { -1.0f,  0.0f,  0.0f },
    {  0.0f,  1.0f,  0.0f }, {  0.0f, -1.0f,  0.0f },
    {  0.0f,  0.0f,  1.0f }, {  0.0f,  0.0f, -1.0f },
};
static uint8_t const OCT_FACES[8][3] = {
    { 0, 2, 4 }, { 0, 2, 5 }, { 0, 3, 4 }, { 0, 3, 5 },
    { 1, 2, 4 }, { 1, 2, 5 }, { 1, 3, 4 }, { 1, 3, 5 },
};

// Draw callback — a red octahedron spinning around Y, one full
// turn per GAME_BOOSTER_ROTATION_PERIOD_S (shared cadence with the
// speed booster). Visible faces are filled two-tone for faceting
// and stroked; back faces are culled.
static void jump_booster_draw(pax_buf_t* fb, obstacle_t const* o, float cam_x) {
    if (o->z_world < RENDER_NEAR_CLIP_Z) return;

    int64_t const now_us = esp_timer_get_time();
    float   const time_s = (float)(now_us % 600000000LL) * 1e-6f;
    float   const angle  = fmodf(time_s, GAME_BOOSTER_ROTATION_PERIOD_S)
                         / GAME_BOOSTER_ROTATION_PERIOD_S * (2.0f * (float)M_PI);
    float const cosa = cosf(angle);
    float const sina = sinf(angle);

    // Scale to the booster footprint; centre at half-height so the
    // body sits on its [y_base, y_base + HEIGHT] collision AABB.
    float const s        = GAME_BOOSTER_HALF_W;
    float const y_centre = o->y_base + GAME_BOOSTER_HEIGHT * 0.5f;

    // Rotated local positions (kept for the centroid cull) + their
    // projected screen coordinates.
    float lvx[6], lvy[6], lvz[6], sx[6], sy[6];
    for (int i = 0; i < 6; i++) {
        float const x  = OCT_VERTS[i][0] * s;
        float const y  = OCT_VERTS[i][1] * s;
        float const z  = OCT_VERTS[i][2] * s;
        float const xr =  x * cosa + z * sina;
        float const zr = -x * sina + z * cosa;
        lvx[i] = xr; lvy[i] = y; lvz[i] = zr;
        render_project(o->x_world + xr, y_centre + y, o->z_world + zr,
                       &sx[i], &sy[i]);
    }

    uint16_t* const fb_pixels = (uint16_t*)pax_buf_get_pixels(fb);
    bool      const rev       = fb->reverse_endianness;
    uint16_t  const out_pk    = direct_565_pack(o->outline_color, rev);
    float     const cam_h     = render_camera().y;

    for (int f = 0; f < 8; f++) {
        int const a = OCT_FACES[f][0];
        int const b = OCT_FACES[f][1];
        int const c = OCT_FACES[f][2];

        // Local centroid = the face's outward normal direction.
        float const cx_l = (lvx[a] + lvx[b] + lvx[c]) * (1.0f / 3.0f);
        float const cy_l = (lvy[a] + lvy[b] + lvy[c]) * (1.0f / 3.0f);
        float const cz_l = (lvz[a] + lvz[b] + lvz[c]) * (1.0f / 3.0f);

        // Back-face cull against the face → camera vector.
        float const dvx = cam_x - (o->x_world + cx_l);
        float const dvy = cam_h - (y_centre   + cy_l);
        float const dvz = 0.0f  - (o->z_world + cz_l);
        if (cx_l * dvx + cy_l * dvy + cz_l * dvz <= 0.0f) continue;

        // Per-face lighting: normalise the centroid (the face's
        // outward normal) and dot it with the light direction. This
        // is what makes the octahedron shimmer as it spins, instead
        // of a fixed bright/dark side.
        float const len  = sqrtf(cx_l * cx_l + cy_l * cy_l + cz_l * cz_l);
        float const inv  = (len > 1e-6f) ? (1.0f / len) : 0.0f;
        float       d    = (cx_l * inv) * JUMP_LIGHT_X
                         + (cy_l * inv) * JUMP_LIGHT_Y
                         + (cz_l * inv) * JUMP_LIGHT_Z;
        if (d < 0.0f) d = 0.0f;
        float const tint = 0.55f + 0.45f * d;

        // Two-tone faceting on a *proper* checkerboard. An octahedron
        // face joins one vertex per axis; the parity of its odd-
        // indexed (negative-axis) vertices flips between every pair
        // of adjacent faces, so no two neighbours share a base
        // colour — unlike a flat `f & 1`, which clumps them.
        uint32_t const base = (((a & 1) + (b & 1) + (c & 1)) & 1)
                                ? o->side_color : o->front_color;
        uint16_t const fill = direct_565_pack(jump_dim(base, tint), rev);

        direct_565_tri(fb_pixels, sx[a], sy[a], sx[b], sy[b], sx[c], sy[c], fill);
        direct_565_line(fb_pixels, (int)sx[a], (int)sy[a], (int)sx[b], (int)sy[b], out_pk);
        direct_565_line(fb_pixels, (int)sx[b], (int)sy[b], (int)sx[c], (int)sy[c], out_pk);
        direct_565_line(fb_pixels, (int)sx[c], (int)sy[c], (int)sx[a], (int)sy[a], out_pk);
    }
}

obstacle_t* jump_booster_spawn_at(world_state_t* w, float x, float z) {
    obstacle_t* const o = obstacle_spawn(
        w, OBSTACLE_KIND_PICKUP_JUMP,
        x, z,
        GAME_BOOSTER_HALF_W, GAME_BOOSTER_HALF_W, GAME_BOOSTER_HEIGHT,
        JUMP_FRONT_COLOR, JUMP_SIDE_COLOR,
        /* top_color (unused — custom draw) */ JUMP_FRONT_COLOR,
        JUMP_OUTLINE_COLOR);
    if (o) o->draw = jump_booster_draw;
    return o;
}

obstacle_t* jump_booster_spawn(world_state_t* w) {
    // Same wall-clearance rule as the speed booster: the outer face
    // stays inside the playfield.
    float const x_extent = TRACK_HALF_WIDTH - GAME_BOOSTER_HALF_W;
    float const x        = (world_frand(&w->stage_prng) * 2.0f - 1.0f) * x_extent;
    return jump_booster_spawn_at(w, x, WORLD_Z_FAR_SPAWN);
}
