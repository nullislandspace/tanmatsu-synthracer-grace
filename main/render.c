#include "render.h"

#include <math.h>

#include "direct_565.h"
#include "esp_timer.h"
#include "magicnumbers.h"
#include "shapes/pax_tris.h"

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
// every entry — dynamic obstacles, side-wall segments, future
// pickups — uniformly. The cube has a square footprint when
// half_w == half_d; non-square footprints (e.g. wall segments
// running along z) work without any special-case code.
//
// Near-plane clipping. When the camera moves into a long obstacle's
// z range (the front edge passes the camera before the back does)
// the projection of the front face heads to infinity. We don't try
// to render anything closer than RENDER_NEAR_CLIP_Z; if the front edge is
// past the clip plane we drop the front face entirely and clip the
// side and top faces' front edges back to RENDER_NEAR_CLIP_Z. The whole
// cube is skipped if the back edge is also past the clip — by then
// the obstacle's despawn condition will fire on the next frame.
// (Constant lives in render.h so custom-draw object modules clip
// their own geometry consistently.)

void render_project(float x_w, float y_w, float z_w, float cam_x, float* out_sx, float* out_sy) {
    if (z_w < 0.01f) z_w = 0.01f;  // guard against /0 if a near-clip slips through
    float const inv_z = 1.0f / z_w;
    *out_sx = RENDER_HALF_W + RENDER_FOCAL_LEN * (x_w - cam_x) * inv_z;
    *out_sy = RENDER_HORIZON_Y - RENDER_FOCAL_LEN * (y_w - RENDER_CAM_Y) * inv_z;
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
        // Per-object shadow callback wins when set. Otherwise default
        // dispatch: only cubes get the trapezoid shadow; walls run
        // along z outside the playfield and pickups / ramps are too
        // short for a meaningful shadow.
        if (o->shadow) {
            o->shadow(fb, o, cam_x, sun_y);
            continue;
        }
        if (o->kind != OBSTACLE_KIND_CUBE) continue;

        float const xL          = o->x_world - o->half_w;
        float const xR          = o->x_world + o->half_w;
        float const z_far       = o->z_world - o->half_d;            // obstacle's near face
        float const shadow_len  = o->height * factor;
        float const z_near_raw  = z_far - shadow_len;                // toward camera

        // Same near-plane clipping rule as render_obstacles: drop
        // the whole shadow if its far edge is already past the
        // near clip; otherwise clip the near edge of the quad to
        // RENDER_NEAR_CLIP_Z so the projection doesn't blow up.
        if (z_far < RENDER_NEAR_CLIP_Z) continue;
        float const z_near = (z_near_raw < RENDER_NEAR_CLIP_Z) ? RENDER_NEAR_CLIP_Z : z_near_raw;

        // Project the four corners of the shadow rectangle on the
        // y = 0 ground plane. The result on screen is a trapezoid
        // (narrow at the obstacle base, wider toward the camera).
        float sx_NL, sy_NL, sx_NR, sy_NR;
        float sx_FL, sy_FL, sx_FR, sy_FR;
        render_project(xL, 0.0f, z_near, cam_x, &sx_NL, &sy_NL);
        render_project(xR, 0.0f, z_near, cam_x, &sx_NR, &sy_NR);
        render_project(xL, 0.0f, z_far,  cam_x, &sx_FL, &sy_FL);
        render_project(xR, 0.0f, z_far,  cam_x, &sx_FR, &sy_FR);

        direct_565_tri(fb_pixels, sx_NL, sy_NL, sx_NR, sy_NR, sx_FR, sy_FR, sh_packed);
        direct_565_tri(fb_pixels, sx_NL, sy_NL, sx_FR, sy_FR, sx_FL, sy_FL, sh_packed);
    }
}

// Draw one pickup as a square-based pyramid (used by Tri pickup —
// Phase 6). Apex sits at (obs.x, obs.height, obs.z); the 4 base
// corners are on the y=0 plane at obs.x±half_w / obs.z±half_d.
// Painter's order inside the pyramid: visible side face first,
// then front. The back face is never visible (camera always
// sits in front and slightly above), so it's skipped. Colours
// come from the obstacle's palette unchanged — no pulse — so
// the same renderer serves any pyramid-shaped pickup that wants
// to override the cube default; the booster used this shape pre-
// Phase 6 but now uses `render_booster_icosahedron` below.
static void render_pickup_pyramid(uint16_t* fb_pixels, obstacle_t const* o, float cam_x,
                                  bool rev_endian) {
    // Whole-pyramid near-plane cull. Pyramids are short in z
    // (half_d = 0.4) and small, so once the apex is at or behind
    // RENDER_NEAR_CLIP_Z just drop the whole thing — saves the
    // projection / clipping math for almost-passed pickups.
    if (o->z_world < RENDER_NEAR_CLIP_Z) return;

    float const xL = o->x_world - o->half_w;
    float const xR = o->x_world + o->half_w;
    float const zF = o->z_world - o->half_d;
    float const zB = o->z_world + o->half_d;
    float const zN = (zF < RENDER_NEAR_CLIP_Z) ? RENDER_NEAR_CLIP_Z : zF;

    // 5 projected vertices: apex + 4 base corners (clockwise from
    // front-left when viewed from above).
    float sx_A,  sy_A;
    float sx_FL, sy_FL, sx_FR, sy_FR;
    float sx_BL, sy_BL, sx_BR, sy_BR;
    render_project(o->x_world, o->height, o->z_world, cam_x, &sx_A,  &sy_A);
    render_project(xL,         0.0f,      zN,          cam_x, &sx_FL, &sy_FL);
    render_project(xR,         0.0f,      zN,          cam_x, &sx_FR, &sy_FR);
    render_project(xL,         0.0f,      zB,          cam_x, &sx_BL, &sy_BL);
    render_project(xR,         0.0f,      zB,          cam_x, &sx_BR, &sy_BR);

    bool const show_left  = cam_x < o->x_world;
    bool const show_right = cam_x > o->x_world;

    uint16_t const front_packed = direct_565_pack(o->front_color, rev_endian);
    uint16_t const side_packed  = direct_565_pack(o->side_color,  rev_endian);
    uint16_t const out_packed   = direct_565_pack(o->outline_color, rev_endian);

    // Side first (whichever's facing the camera), then front. Back
    // face never drawn — camera is always in front of the pickup.
    if (show_left) {
        direct_565_tri(fb_pixels, sx_A, sy_A, sx_FL, sy_FL, sx_BL, sy_BL, side_packed);
    } else if (show_right) {
        direct_565_tri(fb_pixels, sx_A, sy_A, sx_BR, sy_BR, sx_FR, sy_FR, side_packed);
    }
    direct_565_tri(fb_pixels, sx_A, sy_A, sx_FR, sy_FR, sx_FL, sy_FL, front_packed);

    // Wireframe: apex to each base corner of visible faces + the
    // visible base edges. Outline stays bright for a crisp
    // silhouette regardless of the body's fill colour.
    direct_565_line(fb_pixels, (int)sx_A,  (int)sy_A,  (int)sx_FL, (int)sy_FL, out_packed);
    direct_565_line(fb_pixels, (int)sx_A,  (int)sy_A,  (int)sx_FR, (int)sy_FR, out_packed);
    direct_565_line(fb_pixels, (int)sx_FL, (int)sy_FL, (int)sx_FR, (int)sy_FR, out_packed);
    if (show_left) {
        direct_565_line(fb_pixels, (int)sx_A,  (int)sy_A,  (int)sx_BL, (int)sy_BL, out_packed);
        direct_565_line(fb_pixels, (int)sx_FL, (int)sy_FL, (int)sx_BL, (int)sy_BL, out_packed);
    } else if (show_right) {
        direct_565_line(fb_pixels, (int)sx_A,  (int)sy_A,  (int)sx_BR, (int)sy_BR, out_packed);
        direct_565_line(fb_pixels, (int)sx_FR, (int)sy_FR, (int)sx_BR, (int)sy_BR, out_packed);
    }
}

// ----------------------------------------------------------------
// Booster icosahedron (Phase 6 — 2026-05-14 design refresh).
// ----------------------------------------------------------------

// Golden ratio. Regular icosahedron vertices land at
// (0, ±1, ±φ), (±1, ±φ, 0), (±φ, 0, ±1) — all 12 sit at the same
// distance from the origin, ~1.902 = √(2+φ).
#define ICO_PHI 1.618033988749895f

// Local-space vertex coordinates, multiplied by ICO_SCALE at use
// time to fit GAME_BOOSTER_HALF_W. Max abs coordinate is φ, so
// the bounding-box half-extent of the un-scaled icosahedron is
// also φ; ICO_SCALE = HALF_W / φ scales it into the booster's
// collision footprint.
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
// indices. Order chosen so each vertex appears in exactly 5
// faces (verified by topology). Winding doesn't matter for our
// back-face cull (which uses the face centroid direction, not the
// triangle's cross-product normal) — the cull rule is purely
// geometric for a centered convex polyhedron.
static uint8_t const ICO_FACES[20][3] = {
    {  0,  8,  4 }, {  0,  4,  6 }, {  0,  6, 10 }, {  0, 10,  2 }, {  0,  2,  8 },
    {  3,  1,  9 }, {  3, 11,  1 }, {  3,  7, 11 }, {  3,  5,  7 }, {  3,  9,  5 },
    {  4,  8,  9 }, {  1,  4,  9 }, {  6,  4,  1 }, {  1, 11,  6 }, {  6, 11, 10 },
    { 10, 11,  7 }, { 10,  7,  2 }, {  2,  7,  5 }, {  2,  5,  8 }, {  8,  5,  9 },
};

// Render the booster as a rotating regular icosahedron. The
// rotation phase (`angle` in radians) is computed once per frame
// by the caller and shared across all boosters in the scene, so
// every booster spins in lockstep with the global time clock.
//
// Pipeline per booster:
//   1. Rotate all 12 vertices around the Y axis by `angle`.
//   2. Translate to world, project to screen.
//   3. For each face, compute the local rotated centroid; this
//      direction IS the face's outward normal (property of any
//      face centroid on a convex polyhedron centred at origin).
//      Back-face cull via the dot product of that direction with
//      the view direction (face → camera).
//   4. For visible faces, compute a face-normal lighting tint and
//      draw the filled triangle.
//   5. Stroke each visible face's three edges. Edges shared
//      between two visible faces get drawn twice but `direct_565_line`
//      is idempotent and the outline stays crisp.
//
// For a convex polyhedron after back-face culling, visible faces
// don't overlap in 2D projection — no need to z-sort within the
// icosahedron's own faces.
static void render_booster_icosahedron(uint16_t* fb_pixels, obstacle_t const* o, float cam_x,
                                       bool rev_endian, float angle) {
    if (o->z_world < RENDER_NEAR_CLIP_Z) return;

    float const cosa = cosf(angle);
    float const sina = sinf(angle);

    // Y-centre of the icosahedron in world space — collision AABB
    // sits on the ground (y_base = 0, height = HEIGHT), so the
    // icosahedron centres at half-height.
    float const y_centre = GAME_BOOSTER_HEIGHT * 0.5f;

    // Rotated local vertex positions + their projected screen
    // coordinates. Keep both so face centroid math (back-face cull
    // + lighting) can use the rotated local vectors directly.
    float lvx[12], lvy[12], lvz[12];
    float sx[12], sy[12];
    for (int i = 0; i < 12; i++) {
        float const x = ICO_VERTS[i][0] * ICO_SCALE;
        float const y = ICO_VERTS[i][1] * ICO_SCALE;
        float const z = ICO_VERTS[i][2] * ICO_SCALE;

        // Y-axis rotation: (x', y', z') = (x cos + z sin, y, -x sin + z cos).
        float const xr =  x * cosa + z * sina;
        float const yr =  y;
        float const zr = -x * sina + z * cosa;

        lvx[i] = xr; lvy[i] = yr; lvz[i] = zr;

        float const wx = o->x_world + xr;
        float const wy = y_centre   + yr;
        float const wz = o->z_world + zr;
        render_project(wx, wy, wz, cam_x, &sx[i], &sy[i]);
    }

    // Lighting direction (front-top-left, fixed in world space).
    // Picked so the front faces of the icosahedron — the ones the
    // player sees most — sit somewhere between the lit and the
    // mid-tone, making the rotation visibly modulate the brightness
    // of individual faces over time.
    float const light_x = -0.4f;
    float const light_y =  0.7f;
    float const light_z = -0.6f;

    uint16_t const out_packed = direct_565_pack(o->outline_color, rev_endian);

    for (int f = 0; f < 20; f++) {
        int const a = ICO_FACES[f][0];
        int const b = ICO_FACES[f][1];
        int const c = ICO_FACES[f][2];

        // Local-space face centroid (already rotated, since lvx/y/z
        // hold rotated positions). For a centred convex polyhedron,
        // the centroid vector from origin IS the face's outward
        // normal direction — saves a cross product.
        float const cx_l = (lvx[a] + lvx[b] + lvx[c]) * (1.0f / 3.0f);
        float const cy_l = (lvy[a] + lvy[b] + lvy[c]) * (1.0f / 3.0f);
        float const cz_l = (lvz[a] + lvz[b] + lvz[c]) * (1.0f / 3.0f);

        // World-space centroid + view direction (face → camera).
        // Camera sits at (cam_x, RENDER_CAM_Y, 0).
        float const cx_w = o->x_world + cx_l;
        float const cy_w = y_centre   + cy_l;
        float const cz_w = o->z_world + cz_l;
        float const dvx = cam_x         - cx_w;
        float const dvy = RENDER_CAM_Y  - cy_w;
        float const dvz = 0.0f          - cz_w;

        // Back-face cull. Outward normal (cx_l, cy_l, cz_l) needs a
        // positive dot product with the view direction to be facing
        // the camera.
        float const dot_view = cx_l * dvx + cy_l * dvy + cz_l * dvz;
        if (dot_view <= 0.0f) continue;

        // Lighting tint. Normalise the rotated centroid (face
        // centroids on a regular icosahedron all have similar but
        // not identical lengths, so we do this to keep brightness
        // consistent across faces). Then dot with the fixed light
        // direction; positive = lit, clamp at 0.
        float const len = sqrtf(cx_l * cx_l + cy_l * cy_l + cz_l * cz_l);
        float const inv_len = (len > 1e-6f) ? (1.0f / len) : 0.0f;
        float const nx = cx_l * inv_len;
        float const ny = cy_l * inv_len;
        float const nz = cz_l * inv_len;
        float       d  = nx * light_x + ny * light_y + nz * light_z;
        if (d < 0.0f) d = 0.0f;
        float const tint = 0.55f + 0.45f * d;

        // Alternate front_color / side_color by face index so the
        // icosahedron has visible two-tone faceting even before the
        // lighting tint kicks in.
        uint32_t const base_col = (f & 1) ? o->side_color : o->front_color;
        uint16_t const fill_packed = direct_565_pack(dim_argb_render(base_col, tint),
                                                     rev_endian);

        direct_565_tri(fb_pixels,
                       sx[a], sy[a], sx[b], sy[b], sx[c], sy[c],
                       fill_packed);

        // Edges of this face. Pairs that are shared with another
        // visible face will be drawn twice; that's cheaper than
        // building an edge-visibility mask and the line draws are
        // idempotent.
        direct_565_line(fb_pixels, (int)sx[a], (int)sy[a], (int)sx[b], (int)sy[b], out_packed);
        direct_565_line(fb_pixels, (int)sx[b], (int)sy[b], (int)sx[c], (int)sy[c], out_packed);
        direct_565_line(fb_pixels, (int)sx[c], (int)sy[c], (int)sx[a], (int)sy[a], out_packed);
    }
}

void render_obstacles(pax_buf_t* fb, world_state_t const* w, float cam_x) {
    // Build an index list over the active subset, then sort it
    // descending by z so painter's algorithm draws far → near. n is
    // bounded by the pool size (64) so insertion sort is plenty. We
    // sort by the obstacle's centre z_world; obstacles rarely overlap
    // in z so this is a fine proxy for the cube's actual extent.
    int idx[WORLD_OBSTACLE_POOL_SIZE];
    int n = 0;
    for (int i = 0; i < WORLD_OBSTACLE_POOL_SIZE; i++) {
        if (w->obstacles[i].active) idx[n++] = i;
    }
    for (int i = 1; i < n; i++) {
        int   k    = idx[i];
        float zk   = w->obstacles[k].z_world;
        int   j    = i - 1;
        while (j >= 0 && w->obstacles[idx[j]].z_world < zk) {
            idx[j + 1] = idx[j];
            j--;
        }
        idx[j + 1] = k;
    }

    // Wireframe outlines bypass PAX and write straight into the
    // framebuffer halfwords via direct_565_line. The pre-packed
    // colour is recomputed per cube because each obstacle carries
    // its own outline_color, but that's one pack per cube (not per
    // line or per pixel). Triangle fills stay on PAX —
    // pax_range_setter_16bpp is already an optimal halfword memset
    // for solid horizontal runs.
    uint16_t* const fb_pixels = (uint16_t*)pax_buf_get_pixels(fb);
    bool      const rev_endian = fb->reverse_endianness;

    // Booster rotation angle, computed once per frame and shared by
    // every booster so they spin in lockstep. The icosahedron does
    // one full Y-axis rotation per `GAME_BOOSTER_ROTATION_PERIOD_S`
    // (currently 1.0 s). The modulo on now_us keeps the float small
    // so the modf into [0, 1) inside sinf/cosf stays well-conditioned
    // even after the device has been running for hours.
    int64_t const now_us         = esp_timer_get_time();
    float   const time_s         = (float)(now_us % 600000000LL) * 1e-6f;
    float   const booster_angle  = fmodf(time_s, GAME_BOOSTER_ROTATION_PERIOD_S)
                                   / GAME_BOOSTER_ROTATION_PERIOD_S
                                   * (2.0f * (float)M_PI);

    for (int k = 0; k < n; k++) {
        obstacle_t const* o = &w->obstacles[idx[k]];

        // Per-object draw callback wins when set. Otherwise default
        // dispatch: pickup boosters as a rotating icosahedron (Phase
        // 6), Tri pickups as a pyramid (same shape the booster used
        // pre-Phase-6, copied here so the two pickups visually drift
        // apart). Everything else falls through to the cube path.
        if (o->draw) {
            o->draw(fb, o, cam_x);
            continue;
        }
        if (o->kind == OBSTACLE_KIND_PICKUP_BOOST) {
            render_booster_icosahedron(fb_pixels, o, cam_x, rev_endian, booster_angle);
            continue;
        }
        if (o->kind == OBSTACLE_KIND_PICKUP_TRI) {
            render_pickup_pyramid(fb_pixels, o, cam_x, rev_endian);
            continue;
        }

        // Cube extents come straight from the obstacle. z_world is
        // the centre along z; front face sits at zF_raw, back at
        // zB. If the back edge is already past the near clip the
        // whole cube has nothing to draw — bail. Otherwise clip the
        // front edge to RENDER_NEAR_CLIP_Z; if zF_raw was further than the
        // clip we keep the real front face, otherwise we drop the
        // front face and the side/top faces use the clipped near
        // edge instead of the geometric front.
        float const xL     = o->x_world - o->half_w;
        float const xR     = o->x_world + o->half_w;
        float const zF_raw = o->z_world - o->half_d;
        float const zB     = o->z_world + o->half_d;
        float const yB     = o->y_base;
        float const yT     = o->y_base + o->height;
        if (zB < RENDER_NEAR_CLIP_Z) continue;
        bool  const front_visible = (zF_raw >= RENDER_NEAR_CLIP_Z);
        float const zF            = front_visible ? zF_raw : RENDER_NEAR_CLIP_Z;

        // Project the 6 corners we actually need (front face + the
        // back edge of the visible side + the back-top edge for the
        // top face). The two unused back-bottom corners (LBB, RBB)
        // would only be needed if we drew the bottom face, which the
        // default renderer doesn't handle (cubes whose entire body
        // sits above the camera want custom draw callbacks — they're
        // out of scope here).
        float sx_LBF, sy_LBF, sx_RBF, sy_RBF, sx_LTF, sy_LTF, sx_RTF, sy_RTF;
        float sx_LTB, sy_LTB, sx_RTB, sy_RTB, sx_LBB, sy_LBB, sx_RBB, sy_RBB;
        render_project(xL, yB, zF, cam_x, &sx_LBF, &sy_LBF);
        render_project(xR, yB, zF, cam_x, &sx_RBF, &sy_RBF);
        render_project(xL, yT, zF, cam_x, &sx_LTF, &sy_LTF);
        render_project(xR, yT, zF, cam_x, &sx_RTF, &sy_RTF);
        render_project(xL, yT, zB, cam_x, &sx_LTB, &sy_LTB);
        render_project(xR, yT, zB, cam_x, &sx_RTB, &sy_RTB);
        render_project(xL, yB, zB, cam_x, &sx_LBB, &sy_LBB);
        render_project(xR, yB, zB, cam_x, &sx_RBB, &sy_RBB);

        // Visible-face selection. A face is visible when the camera
        // is on the side its outward normal points to.
        bool const show_left  = cam_x < xL;             // camera left of the cube → left face visible
        bool const show_right = cam_x > xR;             // camera right of the cube → right face visible
        bool const show_top   = RENDER_CAM_Y > yT;      // camera above the cube → top face visible

        // Painter's order within the cube: side and top are at least
        // partially deeper than the front, so draw them first. The
        // front face overpaints any sliver of side/top that leaks
        // through to the front edge. Each face uses direct_565_tri:
        // colour pre-packed once per face, scanlines walk in
        // logical-X direction so the inner pixel writes are a
        // contiguous raw-byte run (cache-friendly under ROT_CW).
        if (show_left) {
            uint16_t const c = direct_565_pack(o->side_color, rev_endian);
            direct_565_tri(fb_pixels, sx_LBF, sy_LBF, sx_LTF, sy_LTF, sx_LTB, sy_LTB, c);
            direct_565_tri(fb_pixels, sx_LBF, sy_LBF, sx_LTB, sy_LTB, sx_LBB, sy_LBB, c);
        } else if (show_right) {
            uint16_t const c = direct_565_pack(o->side_color, rev_endian);
            direct_565_tri(fb_pixels, sx_RBF, sy_RBF, sx_RTF, sy_RTF, sx_RTB, sy_RTB, c);
            direct_565_tri(fb_pixels, sx_RBF, sy_RBF, sx_RTB, sy_RTB, sx_RBB, sy_RBB, c);
        }

        if (show_top) {
            uint16_t const c = direct_565_pack(o->top_color, rev_endian);
            direct_565_tri(fb_pixels, sx_LTF, sy_LTF, sx_RTF, sy_RTF, sx_RTB, sy_RTB, c);
            direct_565_tri(fb_pixels, sx_LTF, sy_LTF, sx_RTB, sy_RTB, sx_LTB, sy_LTB, c);
        }

        if (front_visible) {
            uint16_t const c = direct_565_pack(o->front_color, rev_endian);
            direct_565_tri(fb_pixels, sx_LBF, sy_LBF, sx_RBF, sy_RBF, sx_RTF, sy_RTF, c);
            direct_565_tri(fb_pixels, sx_LBF, sy_LBF, sx_RTF, sy_RTF, sx_LTF, sy_LTF, c);
        }

        // Cyan wireframe — each of the cube's 12 edges is drawn
        // exactly once, conditionally on whether it bounds any
        // visible face. Bottom and back faces are never visible
        // (camera is above ground and looking forward), so edges
        // belonging only to those are silently skipped. Listing
        // edge-by-edge avoids the bug where merging the front
        // face's left/right verticals into the side-face branches
        // would drop one of them whenever the camera saw only one
        // side of a tall pillar. Direct-565 line: one halfword
        // store per pixel, no PAX setter dispatch.
        uint16_t const wf = direct_565_pack(o->outline_color, rev_endian);
        if (front_visible)               direct_565_line(fb_pixels, (int)sx_LBF, (int)sy_LBF, (int)sx_RBF, (int)sy_RBF, wf);
        if (front_visible || show_top)   direct_565_line(fb_pixels, (int)sx_LTF, (int)sy_LTF, (int)sx_RTF, (int)sy_RTF, wf);
        if (front_visible || show_left)  direct_565_line(fb_pixels, (int)sx_LBF, (int)sy_LBF, (int)sx_LTF, (int)sy_LTF, wf);
        if (front_visible || show_right) direct_565_line(fb_pixels, (int)sx_RBF, (int)sy_RBF, (int)sx_RTF, (int)sy_RTF, wf);
        if (show_top)                    direct_565_line(fb_pixels, (int)sx_LTB, (int)sy_LTB, (int)sx_RTB, (int)sy_RTB, wf);
        if (show_top || show_left)       direct_565_line(fb_pixels, (int)sx_LTF, (int)sy_LTF, (int)sx_LTB, (int)sy_LTB, wf);
        if (show_top || show_right)      direct_565_line(fb_pixels, (int)sx_RTF, (int)sy_RTF, (int)sx_RTB, (int)sy_RTB, wf);
        if (show_left) {
            direct_565_line(fb_pixels, (int)sx_LBF, (int)sy_LBF, (int)sx_LBB, (int)sy_LBB, wf);
            direct_565_line(fb_pixels, (int)sx_LBB, (int)sy_LBB, (int)sx_LTB, (int)sy_LTB, wf);
        }
        if (show_right) {
            direct_565_line(fb_pixels, (int)sx_RBF, (int)sy_RBF, (int)sx_RBB, (int)sy_RBB, wf);
            direct_565_line(fb_pixels, (int)sx_RBB, (int)sy_RBB, (int)sx_RTB, (int)sy_RTB, wf);
        }
    }
}
