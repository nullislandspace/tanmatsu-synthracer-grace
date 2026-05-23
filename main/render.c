#include "render.h"

#include <math.h>

#include "se_direct565.h"
#include "esp_timer.h"
#include "magicnumbers.h"
#include "scene.h"

// Scale every RGB channel of an ARGB pax_col_t by `scale` (0..1).
// Alpha kept intact. Same shape as the dim_argb helper in game.c
// — duplicated here so render.c doesn't have to pull in game.h.
static inline pax_col_t dim_argb_render(pax_col_t col, float scale) {
    uint32_t const a = (col >> 24) & 0xFF;
    uint32_t const r = (uint32_t)((float)((col >> 16) & 0xFF) * scale);
    uint32_t const g = (uint32_t)((float)((col >>  8) & 0xFF) * scale);
    uint32_t const b = (uint32_t)((float)((col >>  0) & 0xFF) * scale);
    return (a << 24) | (r << 16) | (g << 8) | b;
}

// Per-obstacle dimensions and colours come from the obstacle_t
// itself (world.c sets them at spawn time) so the renderer treats
// every entry — dynamic obstacles, side-wall segments, pickups —
// uniformly. The cube has a square footprint when half_w == half_d;
// non-square footprints (e.g. wall segments running along z) work
// without any special-case code.
//
// Near-plane handling now lives in scene.c: scene_tri / scene_line
// clamp each vertex's z to RENDER_NEAR_CLIP_Z and drop geometry that
// is wholly behind the near plane, so the emitters below just hand
// over raw world-space geometry. The whole-object `z` culls kept
// here are a cheap early-out, not a correctness requirement.

// The camera global. x defaults to track centre, y to the resting
// (grounded) eye height; main.c overwrites both every frame.
static render_camera_t s_camera = { 0.0f, RENDER_CAM_Y };

void render_set_camera(float x, float y) {
    s_camera.x = x;
    s_camera.y = y;
}

render_camera_t render_camera(void) {
    return s_camera;
}

void render_project(float x_w, float y_w, float z_w, float* out_sx, float* out_sy) {
    if (z_w < 0.01f) z_w = 0.01f;  // guard against /0 if a near-clip slips through
    float const inv_z = 1.0f / z_w;
    *out_sx = RENDER_HALF_W + RENDER_FOCAL_LEN * (x_w - s_camera.x) * inv_z;
    *out_sy = RENDER_HORIZON_Y - RENDER_FOCAL_LEN * (y_w - s_camera.y) * inv_z;
}

void render_shadows(pax_buf_t* fb, world_state_t const* w, float cam_x, float sun_y) {
    // After full sunset the floor base is already the shadow
    // colour, so per-obstacle shadow quads would just repaint the
    // same colour. Skip.
    if (sun_y >= GAME_SUN_SINK_RANGE_PX) return;

    float const sun_norm = sun_y / GAME_SUN_SINK_RANGE_PX;
    float const factor   = GAME_SHADOW_LEN_FACTOR_MIN
                         + (GAME_SHADOW_LEN_FACTOR_MAX - GAME_SHADOW_LEN_FACTOR_MIN) * sun_norm;

    uint16_t* const fb_pixels = (uint16_t*)pax_buf_get_pixels(fb);
    uint16_t  const sh_packed = direct_565_pack(GAME_SHADOW_FLOOR_COLOR, fb->reverse_endianness);

    for (int i = 0; i < WORLD_OBSTACLE_POOL_SIZE; i++) {
        obstacle_t const* o = &w->obstacles[i];
        if (!o->active) continue;
        // Per-object shadow callback wins when set (none use it today —
        // the default below is elevation-aware, so the bridge span no
        // longer needs its own). Otherwise default dispatch: only cubes
        // cast a floor shadow; walls run along z outside the playfield
        // and pickups / ramps are too short to matter.
        if (o->shadow) {
            o->shadow(fb, o, cam_x, sun_y);
            continue;
        }
        if (o->kind != OBSTACLE_KIND_CUBE) continue;

        float const xL = o->x_world - o->half_w;
        float const xR = o->x_world + o->half_w;
        // Optimisation: an occluder sitting entirely over a border wall
        // (bridge pillars, sign pillars) throws its shadow onto the wall
        // strip, off the visible playfield floor — skip it.
        if (xR <= -TRACK_HALF_WIDTH || xL >= TRACK_HALF_WIDTH) continue;

        // The floor shadow is the exact dual of the ship's in_shadow ray
        // (game.c): the sun direction is (0, 1, factor), so a floor point
        // z0 at this obstacle's x-span is shadowed iff
        //   z0 in [zN - factor*yTop,  zF - factor*yBase]
        // with zN/zF the obstacle's near/far faces and yBase/yTop its
        // bottom/top. Same `factor` as the gameplay test, so the two
        // never disagree, and the y_base term makes it elevation-aware
        // for free (a raised box's shadow detaches toward the camera).
        float const z_far      = (o->z_world + o->half_d) - factor * o->y_base;
        float const z_near_raw = (o->z_world - o->half_d) - factor * (o->y_base + o->height);

        // Drop the whole shadow if its far edge is already past the near
        // clip; otherwise clamp the near edge so the projection is sane.
        if (z_far < RENDER_NEAR_CLIP_Z) continue;
        float const z_near = (z_near_raw < RENDER_NEAR_CLIP_Z) ? RENDER_NEAR_CLIP_Z : z_near_raw;

        // Project the four corners of the shadow rectangle on the
        // y = 0 ground plane. The result on screen is a trapezoid.
        float sx_NL, sy_NL, sx_NR, sy_NR;
        float sx_FL, sy_FL, sx_FR, sy_FR;
        render_project(xL, 0.0f, z_near, &sx_NL, &sy_NL);
        render_project(xR, 0.0f, z_near, &sx_NR, &sy_NR);
        render_project(xL, 0.0f, z_far,  &sx_FL, &sy_FL);
        render_project(xR, 0.0f, z_far,  &sx_FR, &sy_FR);

        direct_565_tri(fb_pixels, sx_NL, sy_NL, sx_NR, sy_NR, sx_FR, sy_FR, sh_packed);
        direct_565_tri(fb_pixels, sx_NL, sy_NL, sx_FR, sy_FR, sx_FL, sy_FL, sh_packed);
    }
}

// ================================================================
// Geometry emitters.
//
// Each emitter hands its object's geometry to the scene as world-
// space triangles + wireframe edges. No projection, no rasterization,
// no draw-order reasoning — scene.c's z-buffer resolves visibility.
// Per-face camera-side culls are kept purely as a speed optimisation
// (they roughly halve the triangle count); the result is identical
// with or without them because the depth test would discard the
// hidden faces anyway.
// ================================================================

// Tri pickup — a square-based pyramid. Apex at (x, y_base+height, z);
// the four base corners sit on the y_base plane.
static void emit_pyramid(obstacle_t const* o) {
    if (o->z_world < RENDER_NEAR_CLIP_Z) return;

    float const xL = o->x_world - o->half_w;
    float const xR = o->x_world + o->half_w;
    float const zF = o->z_world - o->half_d;
    float const zB = o->z_world + o->half_d;
    float const yB = o->y_base;
    float const xA = o->x_world;
    float const yA = o->y_base + o->height;
    float const zA = o->z_world;

    render_camera_t const cam = render_camera();
    bool const show_left  = cam.x < o->x_world;
    bool const show_right = cam.x > o->x_world;

    uint32_t const fc = o->front_color;
    uint32_t const sc = o->side_color;
    uint32_t const oc = o->outline_color;

    // Front face, then whichever side faces the camera. The back
    // face is never camera-facing, so it is not emitted.
    scene_tri(xA, yA, zA,  xR, yB, zF,  xL, yB, zF,  fc);
    if (show_left)  scene_tri(xA, yA, zA,  xL, yB, zF,  xL, yB, zB,  sc);
    if (show_right) scene_tri(xA, yA, zA,  xR, yB, zB,  xR, yB, zF,  sc);

    scene_line(xA, yA, zA,  xL, yB, zF,  oc);
    scene_line(xA, yA, zA,  xR, yB, zF,  oc);
    scene_line(xL, yB, zF,  xR, yB, zF,  oc);
    if (show_left) {
        scene_line(xA, yA, zA,  xL, yB, zB,  oc);
        scene_line(xL, yB, zF,  xL, yB, zB,  oc);
    }
    if (show_right) {
        scene_line(xA, yA, zA,  xR, yB, zB,  oc);
        scene_line(xR, yB, zF,  xR, yB, zB,  oc);
    }
}

// ----------------------------------------------------------------
// Booster icosahedron.
// ----------------------------------------------------------------

// Golden ratio. Regular icosahedron vertices land at
// (0, ±1, ±φ), (±1, ±φ, 0), (±φ, 0, ±1) — all 12 sit at the same
// distance from the origin, ~1.902 = √(2+φ).
#define ICO_PHI 1.618033988749895f

// Local-space vertex coordinates, multiplied by ICO_SCALE at use
// time to fit GAME_BOOSTER_HALF_W.
#define ICO_SCALE (GAME_BOOSTER_HALF_W / ICO_PHI)

static float const ICO_VERTS[12][3] = {
    { 0.0f,     1.0f,     ICO_PHI },  // v0
    { 0.0f,     1.0f,    -ICO_PHI },  // v1
    { 0.0f,    -1.0f,     ICO_PHI },  // v2
    { 0.0f,    -1.0f,    -ICO_PHI },  // v3
    { 1.0f,     ICO_PHI,  0.0f    },  // v4
    { 1.0f,    -ICO_PHI,  0.0f    },  // v5
    {-1.0f,     ICO_PHI,  0.0f    },  // v6
    {-1.0f,    -ICO_PHI,  0.0f    },  // v7
    { ICO_PHI,  0.0f,     1.0f    },  // v8
    { ICO_PHI,  0.0f,    -1.0f    },  // v9
    {-ICO_PHI,  0.0f,     1.0f    },  // v10
    {-ICO_PHI,  0.0f,    -1.0f    },  // v11
};

// 20 faces of a regular icosahedron, each a triple of vertex
// indices. Winding doesn't matter — the back-face cull uses the
// face centroid direction, not the triangle's cross-product normal.
static uint8_t const ICO_FACES[20][3] = {
    {  0,  8,  4 }, {  0,  4,  6 }, {  0,  6, 10 }, {  0, 10,  2 }, {  0,  2,  8 },
    {  3,  1,  9 }, {  3, 11,  1 }, {  3,  7, 11 }, {  3,  5,  7 }, {  3,  9,  5 },
    {  4,  8,  9 }, {  1,  4,  9 }, {  6,  4,  1 }, {  1, 11,  6 }, {  6, 11, 10 },
    { 10, 11,  7 }, { 10,  7,  2 }, {  2,  7,  5 }, {  2,  5,  8 }, {  8,  5,  9 },
};

// Emit the booster as a rotating regular icosahedron. The rotation
// phase (`angle` in radians) is computed once per frame by the
// caller and shared across all boosters so they spin in lockstep.
//
//   1. Rotate all 12 vertices around the Y axis by `angle`, keeping
//      both the local rotated positions (for the centroid maths) and
//      the world positions (for emission).
//   2. For each face, the local rotated centroid direction IS the
//      face's outward normal (centred convex polyhedron). Back-face
//      cull via the dot product with the view direction.
//   3. For visible faces, compute a face-normal lighting tint and
//      emit the filled triangle + its three edges.
static void emit_icosahedron(obstacle_t const* o, float angle) {
    if (o->z_world < RENDER_NEAR_CLIP_Z) return;

    float const cosa = cosf(angle);
    float const sina = sinf(angle);

    // Y-centre of the icosahedron: the collision AABB is
    // [y_base, y_base + HEIGHT], so it centres at y_base + half-height.
    float const y_centre = o->y_base + GAME_BOOSTER_HEIGHT * 0.5f;

    // Rotated local vertex positions + their world positions.
    float lvx[12], lvy[12], lvz[12];
    float wvx[12], wvy[12], wvz[12];
    for (int i = 0; i < 12; i++) {
        float const x = ICO_VERTS[i][0] * ICO_SCALE;
        float const y = ICO_VERTS[i][1] * ICO_SCALE;
        float const z = ICO_VERTS[i][2] * ICO_SCALE;

        // Y-axis rotation: (x', y', z') = (x cos + z sin, y, -x sin + z cos).
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

        // Local-space (rotated) face centroid — the outward normal
        // direction for a centred convex polyhedron.
        float const cx_l = (lvx[a] + lvx[b] + lvx[c]) * (1.0f / 3.0f);
        float const cy_l = (lvy[a] + lvy[b] + lvy[c]) * (1.0f / 3.0f);
        float const cz_l = (lvz[a] + lvz[b] + lvz[c]) * (1.0f / 3.0f);

        // World-space centroid + view direction (face → camera).
        float const cx_w = o->x_world + cx_l;
        float const cy_w = y_centre   + cy_l;
        float const cz_w = o->z_world + cz_l;
        float const dvx  = cam.x - cx_w;
        float const dvy  = cam.y - cy_w;
        float const dvz  = 0.0f  - cz_w;

        // Back-face cull.
        float const dot_view = cx_l * dvx + cy_l * dvy + cz_l * dvz;
        if (dot_view <= 0.0f) continue;

        // Lighting tint from the normalised face normal.
        float const len     = sqrtf(cx_l * cx_l + cy_l * cy_l + cz_l * cz_l);
        float const inv_len = (len > 1e-6f) ? (1.0f / len) : 0.0f;
        float const nx = cx_l * inv_len;
        float const ny = cy_l * inv_len;
        float const nz = cz_l * inv_len;
        float       d  = nx * light_x + ny * light_y + nz * light_z;
        if (d < 0.0f) d = 0.0f;
        float const tint = 0.55f + 0.45f * d;

        // Alternate front_color / side_color by face index for a
        // visible two-tone faceting.
        uint32_t const base_col = (f & 1) ? o->side_color : o->front_color;

        scene_tri(wvx[a], wvy[a], wvz[a],
                  wvx[b], wvy[b], wvz[b],
                  wvx[c], wvy[c], wvz[c],
                  dim_argb_render(base_col, tint));

        scene_line(wvx[a], wvy[a], wvz[a], wvx[b], wvy[b], wvz[b], o->outline_color);
        scene_line(wvx[b], wvy[b], wvz[b], wvx[c], wvy[c], wvz[c], o->outline_color);
        scene_line(wvx[c], wvy[c], wvz[c], wvx[a], wvy[a], wvz[a], o->outline_color);
    }
}

// Default cube / wall / gate-slab. y range is [y_base, y_base+height].
static void emit_cube(obstacle_t const* o) {
    float const xL = o->x_world - o->half_w;
    float const xR = o->x_world + o->half_w;
    float const zF = o->z_world - o->half_d;
    float const zB = o->z_world + o->half_d;
    float const yB = o->y_base;
    float const yT = o->y_base + o->height;
    if (zB < RENDER_NEAR_CLIP_Z) return;

    render_camera_t const cam = render_camera();
    bool const show_left   = cam.x < xL;   // left face's normal faces the camera
    bool const show_right  = cam.x > xR;
    bool const show_top    = cam.y > yT;
    bool const show_bottom = cam.y < yB;

    uint32_t const fc = o->front_color;
    uint32_t const sc = o->side_color;
    uint32_t const tc = o->top_color;
    uint32_t const oc = o->outline_color;

    // Front face (-z normal) always faces the camera (camera is at
    // z = 0, the cube is at z > 0). The other faces are emitted only
    // when camera-facing — a pure speed optimisation.
    scene_tri(xL, yB, zF,  xR, yB, zF,  xR, yT, zF,  fc);
    scene_tri(xL, yB, zF,  xR, yT, zF,  xL, yT, zF,  fc);
    if (show_left) {
        scene_tri(xL, yB, zF,  xL, yT, zF,  xL, yT, zB,  sc);
        scene_tri(xL, yB, zF,  xL, yT, zB,  xL, yB, zB,  sc);
    }
    if (show_right) {
        scene_tri(xR, yB, zF,  xR, yT, zF,  xR, yT, zB,  sc);
        scene_tri(xR, yB, zF,  xR, yT, zB,  xR, yB, zB,  sc);
    }
    if (show_top) {
        scene_tri(xL, yT, zF,  xR, yT, zF,  xR, yT, zB,  tc);
        scene_tri(xL, yT, zF,  xR, yT, zB,  xL, yT, zB,  tc);
    }
    if (show_bottom) {
        scene_tri(xL, yB, zF,  xR, yB, zF,  xR, yB, zB,  sc);
        scene_tri(xL, yB, zF,  xR, yB, zB,  xL, yB, zB,  sc);
    }

    // All 12 edges, emitted unconditionally. The depth test (with the
    // small edge bias) hides whichever edges a face occludes, so the
    // old per-edge visibility bookkeeping is no longer needed.
    scene_line(xL, yB, zF,  xR, yB, zF,  oc);   // front quad
    scene_line(xR, yB, zF,  xR, yT, zF,  oc);
    scene_line(xR, yT, zF,  xL, yT, zF,  oc);
    scene_line(xL, yT, zF,  xL, yB, zF,  oc);
    scene_line(xL, yB, zB,  xR, yB, zB,  oc);   // back quad
    scene_line(xR, yB, zB,  xR, yT, zB,  oc);
    scene_line(xR, yT, zB,  xL, yT, zB,  oc);
    scene_line(xL, yT, zB,  xL, yB, zB,  oc);
    scene_line(xL, yB, zF,  xL, yB, zB,  oc);   // connectors
    scene_line(xR, yB, zF,  xR, yB, zB,  oc);
    scene_line(xL, yT, zF,  xL, yT, zB,  oc);
    scene_line(xR, yT, zF,  xR, yT, zB,  oc);
}

void render_submit_obstacles(world_state_t const* w) {
    // Booster rotation angle, computed once per frame and shared by
    // every booster so they spin in lockstep. One full Y-axis
    // rotation per GAME_BOOSTER_ROTATION_PERIOD_S. The modulo on
    // now_us keeps the float small so the fmodf stays well-conditioned
    // even after the device has been running for hours.
    int64_t const now_us        = esp_timer_get_time();
    float   const time_s        = (float)(now_us % 600000000LL) * 1e-6f;
    float   const booster_angle = fmodf(time_s, GAME_BOOSTER_ROTATION_PERIOD_S)
                                  / GAME_BOOSTER_ROTATION_PERIOD_S
                                  * (2.0f * (float)M_PI);

    // No sort — the scene's z-buffer resolves visibility per pixel,
    // so obstacles are emitted in pool order.
    for (int i = 0; i < WORLD_OBSTACLE_POOL_SIZE; i++) {
        obstacle_t const* o = &w->obstacles[i];
        if (!o->active) continue;

        // Per-object emit callback wins when set. Otherwise default
        // dispatch: pickup boosters as a rotating icosahedron, Tri
        // pickups as a pyramid, everything else as a cube.
        if (o->emit) {
            o->emit(o);
            continue;
        }
        switch (o->kind) {
            case OBSTACLE_KIND_PICKUP_BOOST: emit_icosahedron(o, booster_angle); break;
            case OBSTACLE_KIND_PICKUP_TRI:   emit_pyramid(o);                    break;
            default:                         emit_cube(o);                      break;
        }
    }
}
