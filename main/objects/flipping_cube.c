#include "objects/flipping_cube.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "direct_565.h"
#include "magicnumbers.h"
#include "render.h"   // render_project, RENDER_NEAR_CLIP_Z, RENDER_CAM_Y
#include "sfx/sfx_cube_bump.h"
#include "world.h"

// Body palette — a dark blue cube with brighter blue highlights on
// the visually-up face. Outline colour is per-direction (red for
// left-roll, green for right-roll) and travels into the obstacle's
// outline_color slot at spawn time.
#define FLIP_FRONT_COLOR        0xFF4080FFu
#define FLIP_SIDE_COLOR         0xFF2050B0u
#define FLIP_TOP_COLOR          0xFF80B0FFu
#define FLIP_OUTLINE_LEFT       0xFFFF3030u   // red — pivots on bottom-LEFT edge
#define FLIP_OUTLINE_RIGHT      0xFF30FF30u   // green — pivots on bottom-RIGHT edge

// Per-object state living in the obstacle's 128-byte scratch buffer.
// Stored here rather than in obstacle_t fields because it's specific
// to this object type. progress is a cached value so the renderer
// doesn't have to redo the z→t mapping the physics callback already
// did this frame.
typedef struct {
    int8_t  direction;     // -1 left-roll, +1 right-roll
    uint8_t pad[2];
    bool    landed_fired;  // true after the one-shot landing SFX has played
    float   progress;      // 0 = upright, 1 = fully rolled onto side
    float   x_initial;     // original upright-cube center x (pivot
                           // x = x_initial + direction * HALF_W)
} flipping_state_t;
_Static_assert(sizeof(flipping_state_t) <= 128, "flipping_state_t exceeds obstacle scratch");

// Compute the 4 cross-section corners (in world-x, world-y) of the
// rotated body. The cross-section is the cube's slice at any fixed
// z — a rectangle in (x, y) at progress=0, rotated 90° around the
// pivot edge at progress=1. Caller-supplied `out` is filled in
// CCW order starting from the original bottom-left corner.
static void flipping_compute_corners(flipping_state_t const* s, float out_x[4], float out_y[4]) {
    float const hw    = FLIPPING_CUBE_HALF_W;
    float const ht    = FLIPPING_CUBE_HEIGHT;
    float const angle = s->progress * (float)M_PI_2;        // 0..π/2
    // Roll direction: left-roll (dir=-1) = CCW about pivot = +angle;
    // right-roll (dir=+1) = CW about pivot = -angle.
    float const rot   = -(float)s->direction * angle;
    float const c     = cosf(rot);
    float const sn    = sinf(rot);

    // Pivot in world coords sits on the ground (y=0) at the
    // cube's roll-direction side. The cube's centre x stays at
    // x_initial for the pivot-frame math; the rendered/colliding
    // centre after rotation comes out of the corner positions.
    float const px = s->x_initial + (float)s->direction * hw;
    float const py = 0.0f;

    // Original (upright) corners in world coords, CCW from BL.
    float const orig_x[4] = { s->x_initial - hw, s->x_initial + hw,
                              s->x_initial + hw, s->x_initial - hw };
    float const orig_y[4] = { 0.0f, 0.0f, ht, ht };

    for (int i = 0; i < 4; i++) {
        float const dx = orig_x[i] - px;
        float const dy = orig_y[i] - py;
        out_x[i] = px + dx * c - dy * sn;
        out_y[i] = py + dx * sn + dy * c;
    }
}

// Physics callback — runs every frame, before collision. Maps the
// cube's current z to a roll progress, then refreshes the obstacle's
// AABB (x_world, half_w, height) so collision and the default shadow
// renderer see the rotated footprint without any extra plumbing.
// z_world and half_d aren't touched (the cube rolls along its z-axis,
// so its z extent is unchanged).
static void flipping_physics(obstacle_t* o, world_state_t* w, float dt, float cam_x) {
    (void)w; (void)dt; (void)cam_x;
    flipping_state_t* s = (flipping_state_t*)o->scratch;

    // Linear ramp from START_Z down to END_Z. We don't ease this —
    // a moving cube already has its perceived speed modulated by
    // the camera's approach, and an easing curve on top would feel
    // squishy.
    float t = (FLIPPING_CUBE_ROLL_START_Z - o->z_world)
            / (FLIPPING_CUBE_ROLL_START_Z - FLIPPING_CUBE_ROLL_END_Z);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    // Edge-trigger: fire the deep bump exactly once, on the frame
    // the cube hits its final orientation. Subsequent frames clamp
    // to 1.0 but stay quiet.
    if (t >= 1.0f && !s->landed_fired) {
        s->landed_fired = true;
        sfx_cube_bump_play();
    }
    s->progress = t;

    // Compute current AABB of the rotated cross-section. The pivot
    // is always on the ground (y=0), so the min-y is always 0.
    float cx[4], cy[4];
    flipping_compute_corners(s, cx, cy);
    float min_x = cx[0], max_x = cx[0], max_y = cy[0];
    for (int i = 1; i < 4; i++) {
        if (cx[i] < min_x) min_x = cx[i];
        if (cx[i] > max_x) max_x = cx[i];
        if (cy[i] > max_y) max_y = cy[i];
    }

    // Update the obstacle's collision footprint. x_world becomes the
    // AABB center (so collision's (x_world ± half_w) covers the body
    // correctly), height becomes the AABB top so the default shadow
    // length scales with how tilted the cube is, half_w spans the
    // full lateral extent. y_base stays 0 because the pivot keeps
    // touching the ground.
    o->x_world = 0.5f * (min_x + max_x);
    o->half_w  = 0.5f * (max_x - min_x);
    o->height  = max_y;
    o->y_base  = 0.0f;
}

// Custom draw — projects the 8 rotated corners (4 cross-section ×
// 2 z faces) and fills the visible side faces + the front face.
// Same near-plane clipping rule as the default cube renderer:
// drop the whole cube if the back face is already past the clip,
// clip the front face to RENDER_NEAR_CLIP_Z otherwise.
//
// Face visibility uses the rotated 2D normal of each cross-section
// edge vs the camera-edge midpoint vector. The "back" face (z=zB)
// is never visible from a forward-pointing camera at z=0 with
// obstacles at z > 0, so we skip it.
static void flipping_draw(pax_buf_t* fb, obstacle_t const* o, float cam_x) {
    flipping_state_t const* s = (flipping_state_t const*)o->scratch;

    float const zF_raw = o->z_world - o->half_d;
    float const zB     = o->z_world + o->half_d;
    if (zB < RENDER_NEAR_CLIP_Z) return;
    bool  const front_visible = (zF_raw >= RENDER_NEAR_CLIP_Z);
    float const zF            = front_visible ? zF_raw : RENDER_NEAR_CLIP_Z;

    float cx[4], cy[4];
    flipping_compute_corners(s, cx, cy);

    // 8 projected corners: cross-section × (front z, back z).
    float sxF[4], syF[4], sxB[4], syB[4];
    for (int i = 0; i < 4; i++) {
        render_project(cx[i], cy[i], zF, cam_x, &sxF[i], &syF[i]);
        render_project(cx[i], cy[i], zB, cam_x, &sxB[i], &syB[i]);
    }

    uint16_t* const fb_pixels = (uint16_t*)pax_buf_get_pixels(fb);
    bool      const rev       = fb->reverse_endianness;
    uint16_t  const front_pk  = direct_565_pack(o->front_color,   rev);
    uint16_t  const side_pk   = direct_565_pack(o->side_color,    rev);
    uint16_t  const top_pk    = direct_565_pack(o->top_color,     rev);
    uint16_t  const out_pk    = direct_565_pack(o->outline_color, rev);

    // Determine which side faces are visible. The cross-section is
    // listed CCW, so the outward normal of edge i→(i+1) is the
    // right-perpendicular of the edge vector:
    //   edge = (B-A); normal = (edge.y, -edge.x)
    // The face is visible iff `normal · (camera_xy - edge_mid_xy) > 0`.
    // Camera sits at (cam_x, RENDER_CAM_Y).
    //
    // Face role tagging — used for colour choice. The originally-top
    // edge (TR→TL = index 2) gets the top palette so the highlighted
    // surface follows the cube as it tips. Both adjacent vertical
    // edges share the standard side palette. The bottom edge
    // (BL→BR = index 0) is the cube's contact with the ground; if
    // the rotation lifts it visibly into view it borrows side palette.
    uint16_t const face_color[4] = { side_pk, side_pk, top_pk, side_pk };
    bool show[4];
    for (int i = 0; i < 4; i++) {
        int   const j   = (i + 1) & 3;
        float const ex  = cx[j] - cx[i];
        float const ey  = cy[j] - cy[i];
        float const nx  = ey;
        float const ny  = -ex;
        float const mx  = 0.5f * (cx[i] + cx[j]);
        float const my  = 0.5f * (cy[i] + cy[j]);
        show[i] = (nx * (cam_x - mx) + ny * (RENDER_CAM_Y - my)) > 0.0f;
    }

    // Painter's order: side faces first (they're partially behind
    // the front face), then the front face overpaints any sliver
    // bleeding through to the front edge. Wireframe last so the
    // outline stays crisp.
    for (int i = 0; i < 4; i++) {
        if (!show[i]) continue;
        int const j = (i + 1) & 3;
        direct_565_tri(fb_pixels,
                       sxF[i], syF[i], sxF[j], syF[j], sxB[j], syB[j],
                       face_color[i]);
        direct_565_tri(fb_pixels,
                       sxF[i], syF[i], sxB[j], syB[j], sxB[i], syB[i],
                       face_color[i]);
    }

    if (front_visible) {
        // Cross-section is a (rotated) convex quad — fan from corner
        // 0 covers it with two triangles.
        direct_565_tri(fb_pixels,
                       sxF[0], syF[0], sxF[1], syF[1], sxF[2], syF[2],
                       front_pk);
        direct_565_tri(fb_pixels,
                       sxF[0], syF[0], sxF[2], syF[2], sxF[3], syF[3],
                       front_pk);
    }

    // Wireframe. The outline_color carries the per-subtype red/green
    // tint, so drawing all visible edges is what makes the two
    // subtypes visually distinct. Front quad always (if visible),
    // back quad edge per visible side face, plus the connecting
    // verticals at the shared corners.
    if (front_visible) {
        for (int i = 0; i < 4; i++) {
            int const j = (i + 1) & 3;
            direct_565_line(fb_pixels,
                            (int)sxF[i], (int)syF[i],
                            (int)sxF[j], (int)syF[j], out_pk);
        }
    }
    for (int i = 0; i < 4; i++) {
        if (!show[i]) continue;
        int const j = (i + 1) & 3;
        direct_565_line(fb_pixels,
                        (int)sxB[i], (int)syB[i],
                        (int)sxB[j], (int)syB[j], out_pk);
        direct_565_line(fb_pixels,
                        (int)sxF[i], (int)syF[i],
                        (int)sxB[i], (int)syB[i], out_pk);
        direct_565_line(fb_pixels,
                        (int)sxF[j], (int)syF[j],
                        (int)sxB[j], (int)syB[j], out_pk);
    }
}

obstacle_t* flipping_cube_spawn(world_state_t* w, float x, float z, int direction) {
    uint32_t const outline_color = (direction < 0) ? FLIP_OUTLINE_LEFT : FLIP_OUTLINE_RIGHT;
    obstacle_t* const o = obstacle_spawn(w, OBSTACLE_KIND_CUBE,
                                         x, z,
                                         FLIPPING_CUBE_HALF_W,
                                         FLIPPING_CUBE_HALF_D,
                                         FLIPPING_CUBE_HEIGHT,
                                         FLIP_FRONT_COLOR, FLIP_SIDE_COLOR,
                                         FLIP_TOP_COLOR,   outline_color);
    if (!o) return NULL;
    flipping_state_t* s = (flipping_state_t*)o->scratch;
    s->direction    = (direction < 0) ? -1 : +1;
    s->progress     = 0.0f;
    s->x_initial    = x;
    s->landed_fired = false;
    o->physics   = flipping_physics;
    o->draw      = flipping_draw;
    // collide left at NULL → default OBSTACLE_KIND_CUBE dispatch:
    //   head-on while the player approaches the upright cube, scrape
    //   if it lands and the player's coming around its trailing edge.
    // shadow left at NULL → default cube shadow uses the updated
    //   (x_world, half_w, height) AABB so the ground shadow tracks
    //   the roll automatically.
    return o;
}
