#include "objects/restarea_marker.h"

#include <math.h>
#include <stdint.h>

#include "esp_timer.h"
#include "objects/restarea_marker_model.h"
#include "objects/wall.h"   // WALL_X_LEFT / WALL_X_RIGHT / WALL_HALF_W / WALL_HEIGHT
#include "render.h"         // render_camera, RENDER_NEAR_CLIP_Z
#include "se_scene.h"          // scene_tri, scene_line
#include "world.h"

// Beacon pulse: full period in seconds (full green → grey → full green).
#define MARKER_PULSE_PERIOD_S  1.0f
// Dim end of the beacon pulse — the beacon fades fully to black.
#define MARKER_BEACON_DARK     0xFF000000u

// Multiply each RGB channel of an ARGB colour by `s` (0..1); alpha kept.
static inline uint32_t marker_dim(uint32_t col, float s) {
    uint32_t const r = (uint32_t)((float)((col >> 16) & 0xFFu) * s);
    uint32_t const g = (uint32_t)((float)((col >>  8) & 0xFFu) * s);
    uint32_t const b = (uint32_t)((float)((col >>  0) & 0xFFu) * s);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

// Per-channel lerp between two opaque ARGB colours: t=0 → a, t=1 → b.
static inline uint32_t marker_lerp(uint32_t a, uint32_t b, float t) {
    int const ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    int const br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    int const r = ar + (int)((float)(br - ar) * t);
    int const g = ag + (int)((float)(bg - ag) * t);
    int const bl = ab + (int)((float)(bb - ab) * t);
    return 0xFF000000u | (r << 16) | (g << 8) | bl;
}

// Emit callback — a static post (no spin/bank). The grey post is lit
// per-face so it reads as 3D; the green beacon is drawn flat and pulses.
static void restarea_marker_emit(obstacle_t const* o) {
    if (o->z_world < RENDER_NEAR_CLIP_Z) return;

    float const s = RESTMARK_SCALE;
    float wx[RESTMARK_VERT_COUNT], wy[RESTMARK_VERT_COUNT], wz[RESTMARK_VERT_COUNT];
    for (size_t i = 0; i < RESTMARK_VERT_COUNT; i++) {
        restmark_vert_t const* v = &RESTMARK_VERTS[i];
        // Base-anchored model: local y = 0 sits on the wall top (y_base).
        wx[i] = o->x_world + v->x * s;
        wy[i] = o->y_base  + v->y * s + RESTMARK_Y_OFFSET;
        wz[i] = o->z_world + v->z * s + RESTMARK_Z_OFFSET;
    }

    // Beacon pulse: 1.0 = full green at the cycle start, dipping to 0.0
    // (black) at the half-cycle and back. cos gives the smooth fade.
    int64_t const now_us = esp_timer_get_time();
    float   const phase  = (float)(now_us % (int64_t)(MARKER_PULSE_PERIOD_S * 1e6f))
                         / (MARKER_PULSE_PERIOD_S * 1e6f);
    float   const pulse  = 0.5f * (1.0f + cosf(phase * 2.0f * (float)M_PI));
    uint32_t const beacon_fill =
        marker_lerp(MARKER_BEACON_DARK, RESTMARK_REGION_FILL[RESTMARK_REGION_BEACON], pulse);

    // Fixed world-space light (front-top-left), matching the rest of the
    // hard-surface objects.
    float const light_x = -0.4f, light_y = 0.7f, light_z = -0.6f;
    render_camera_t const cam = render_camera();

    for (size_t i = 0; i < RESTMARK_TRI_COUNT; i++) {
        restmark_tri_t const* t = &RESTMARK_TRIS[i];
        int const a = t->a, b = t->b, c = t->c;

        // CCW-outward face normal from the two edges.
        float const ux = wx[b] - wx[a], uy = wy[b] - wy[a], uz = wz[b] - wz[a];
        float const vx = wx[c] - wx[a], vy = wy[c] - wy[a], vz = wz[c] - wz[a];
        float const nx = uy * vz - uz * vy;
        float const ny = uz * vx - ux * vz;
        float const nz = ux * vy - uy * vx;

        // Back-face cull against the camera at (cam.x, cam.y, 0).
        float const fcx = (wx[a] + wx[b] + wx[c]) * (1.0f / 3.0f);
        float const fcy = (wy[a] + wy[b] + wy[c]) * (1.0f / 3.0f);
        float const fcz = (wz[a] + wz[b] + wz[c]) * (1.0f / 3.0f);
        if (nx * (cam.x - fcx) + ny * (cam.y - fcy) + nz * (0.0f - fcz) <= 0.0f) {
            continue;
        }

        uint32_t col;
        if (t->region == RESTMARK_REGION_BEACON) {
            col = beacon_fill;  // flat, pulsing
        } else {
            // Per-face directional lighting on the grey post.
            float const nlen = sqrtf(nx * nx + ny * ny + nz * nz);
            float const inv  = (nlen > 1e-6f) ? (1.0f / nlen) : 0.0f;
            float       d    = (nx * inv) * light_x + (ny * inv) * light_y + (nz * inv) * light_z;
            if (d < 0.0f) d = 0.0f;
            col = marker_dim(RESTMARK_REGION_FILL[t->region], 0.55f + 0.45f * d);
        }

        scene_tri(wx[a], wy[a], wz[a],
                  wx[b], wy[b], wz[b],
                  wx[c], wy[c], wz[c], col);
    }

    // Outline: per-region colour from the model — white post, neutral
    // dark-grey beacon (the beacon outline does NOT pulse, only its fill
    // does, fading the green fully to black).
    for (size_t i = 0; i < RESTMARK_EDGE_COUNT; i++) {
        restmark_edge_t const* e = &RESTMARK_EDGES[i];
        scene_line(wx[e->a], wy[e->a], wz[e->a],
                   wx[e->b], wy[e->b], wz[e->b],
                   RESTMARK_REGION_OUTLINE[e->region]);
    }
}

static void spawn_marker(world_state_t* w, float x, float z) {
    obstacle_t* const m = obstacle_spawn(
        w, OBSTACLE_KIND_CUBE,
        x, z,
        WALL_HALF_W, WALL_HALF_W, 2.0f,
        RESTMARK_REGION_FILL[RESTMARK_REGION_POST], RESTMARK_REGION_FILL[RESTMARK_REGION_POST],
        RESTMARK_REGION_FILL[RESTMARK_REGION_POST], RESTMARK_REGION_OUTLINE[RESTMARK_REGION_POST]);
    // Stand on the wall top; custom emit renders it, so the colours
    // above are only fallbacks. Sits at the wall x, fully outside the
    // ship's reachable range, so it never collides (same as the old
    // posts — no collide override needed).
    if (m) {
        m->y_base = WALL_HEIGHT;
        m->emit   = restarea_marker_emit;
    }
}

void restarea_marker_pair_spawn(world_state_t* w, float z) {
    // Nudged 0.01 u toward the camera so it sits cleanly in front of any
    // co-located wall segment (matches the old rest posts).
    float const marker_z = z - 0.01f;
    spawn_marker(w, WALL_X_LEFT,  marker_z);
    spawn_marker(w, WALL_X_RIGHT, marker_z);
}
