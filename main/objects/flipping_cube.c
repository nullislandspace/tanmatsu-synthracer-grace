#include "objects/flipping_cube.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "magicnumbers.h"
#include "render.h"   // render_camera, RENDER_NEAR_CLIP_Z
#include "scene.h"    // scene_tri, scene_line
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

// Emit callback — hands the prism's geometry (4 rotated cross-section
// corners × front/back z faces) to the scene as world-space triangles
// and edges. Side faces are emitted only when camera-facing (a speed
// optimisation); the front face is always emitted and the depth
// buffer resolves occlusion against everything else.
//
// Side-face visibility uses the rotated 2D normal of each cross-
// section edge vs the camera-edge midpoint vector.
static void flipping_emit(obstacle_t const* o) {
    flipping_state_t const* s = (flipping_state_t const*)o->scratch;

    float const zF = o->z_world - o->half_d;   // near face
    float const zB = o->z_world + o->half_d;   // far face
    if (zB < RENDER_NEAR_CLIP_Z) return;

    float cx[4], cy[4];
    flipping_compute_corners(s, cx, cy);

    uint32_t const front_c = o->front_color;
    uint32_t const side_c  = o->side_color;
    uint32_t const top_c   = o->top_color;
    uint32_t const out_c   = o->outline_color;

    // Face role tagging — used for colour choice. The originally-top
    // edge (TR→TL = index 2) gets the top palette so the highlighted
    // surface follows the cube as it tips; the others use the side
    // palette.
    uint32_t const face_color[4] = { side_c, side_c, top_c, side_c };

    // Which side faces are camera-facing. The cross-section is listed
    // CCW, so the outward normal of edge i→(i+1) is the right-
    // perpendicular of the edge vector: normal = (edge.y, -edge.x).
    // Visible iff normal · (camera_xy - edge_mid_xy) > 0.
    render_camera_t const cam = render_camera();
    bool show[4];
    for (int i = 0; i < 4; i++) {
        int   const j   = (i + 1) & 3;
        float const ex  = cx[j] - cx[i];
        float const ey  = cy[j] - cy[i];
        float const nx  = ey;
        float const ny  = -ex;
        float const mx  = 0.5f * (cx[i] + cx[j]);
        float const my  = 0.5f * (cy[i] + cy[j]);
        show[i] = (nx * (cam.x - mx) + ny * (cam.y - my)) > 0.0f;
    }

    // Side faces — quad F[i] F[j] B[j] B[i], two triangles each.
    for (int i = 0; i < 4; i++) {
        if (!show[i]) continue;
        int const j = (i + 1) & 3;
        scene_tri(cx[i], cy[i], zF,  cx[j], cy[j], zF,  cx[j], cy[j], zB,
                  face_color[i]);
        scene_tri(cx[i], cy[i], zF,  cx[j], cy[j], zB,  cx[i], cy[i], zB,
                  face_color[i]);
    }

    // Front face — the (rotated) convex cross-section quad, fanned
    // from corner 0.
    scene_tri(cx[0], cy[0], zF,  cx[1], cy[1], zF,  cx[2], cy[2], zF,  front_c);
    scene_tri(cx[0], cy[0], zF,  cx[2], cy[2], zF,  cx[3], cy[3], zF,  front_c);

    // Wireframe. The outline_color carries the per-subtype red/green
    // tint. Front quad always; back-quad edge + connecting verticals
    // for each visible side face.
    for (int i = 0; i < 4; i++) {
        int const j = (i + 1) & 3;
        scene_line(cx[i], cy[i], zF,  cx[j], cy[j], zF,  out_c);
    }
    for (int i = 0; i < 4; i++) {
        if (!show[i]) continue;
        int const j = (i + 1) & 3;
        scene_line(cx[i], cy[i], zB,  cx[j], cy[j], zB,  out_c);
        scene_line(cx[i], cy[i], zF,  cx[i], cy[i], zB,  out_c);
        scene_line(cx[j], cy[j], zF,  cx[j], cy[j], zB,  out_c);
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
    o->emit      = flipping_emit;
    // collide left at NULL → default OBSTACLE_KIND_CUBE dispatch:
    //   head-on while the player approaches the upright cube, scrape
    //   if it lands and the player's coming around its trailing edge.
    // shadow left at NULL → default cube shadow uses the updated
    //   (x_world, half_w, height) AABB so the ground shadow tracks
    //   the roll automatically.
    return o;
}
