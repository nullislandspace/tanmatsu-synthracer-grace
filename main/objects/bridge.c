#include "objects/bridge.h"

#include "direct_565.h"
#include "magicnumbers.h"
#include "objects/wall.h"   // for WALL_X_LEFT / WALL_X_RIGHT / WALL_HALF_W
#include "render.h"         // for render_project, RENDER_NEAR_CLIP_Z
#include "world.h"

// Concrete-grey palette. Distinct from the magenta walls and amber
// gate slabs so bridges read as "infrastructure" rather than danger.
#define CONCRETE_FRONT_COLOR    0xFF989898u
#define CONCRETE_SIDE_COLOR     0xFF606060u
#define CONCRETE_TOP_COLOR      0xFFB0B0B0u
#define CONCRETE_OUTLINE_COLOR  0xFF31FBFBu

// Span half-width: spans wall-to-wall, covering the full track plus
// the wall thickness on either side. Rendered so its top and front
// faces are visible; the underside is the face the player flies
// beneath.
#define SPAN_HALF_W             (TRACK_HALF_WIDTH + WALL_HALF_W)

// --- Span draw callback --------------------------------------------
// The span sits at y in [BRIDGE_SPAN_Y_BASE, BRIDGE_SPAN_Y_BASE +
// BRIDGE_SPAN_HEIGHT]. The default cube renderer assumes y=0 base
// and can't represent elevation, so we draw the span manually:
// project the 8 corners we need (4 bottom + 4 top), then fill the
// visible faces. Camera at y = RENDER_CAM_Y (= 1) sits *below* the
// span, so the visible faces are: bottom (looking up), front
// (toward camera in z). Side faces are never visible because the
// span spans the full playfield laterally — cam_x is always inside
// the span's x range. The back face never faces the camera.
static void span_draw(pax_buf_t* fb, obstacle_t const* o, float cam_x) {
    (void)cam_x;  // span is full-width; render_project reads the camera global
    float const xL = o->x_world - o->half_w;
    float const xR = o->x_world + o->half_w;
    float const zF_raw = o->z_world - o->half_d;
    float const zB     = o->z_world + o->half_d;
    if (zB < RENDER_NEAR_CLIP_Z) return;
    float const zF = (zF_raw < RENDER_NEAR_CLIP_Z) ? RENDER_NEAR_CLIP_Z : zF_raw;
    bool  const front_visible = (zF_raw >= RENDER_NEAR_CLIP_Z);

    float const yB = BRIDGE_SPAN_Y_BASE;
    float const yT = BRIDGE_SPAN_Y_BASE + o->height;

    // 8 corners: L/R × B(ottom)/T(op) × F(ront)/B(ack).
    float sx_LBF, sy_LBF, sx_RBF, sy_RBF, sx_LTF, sy_LTF, sx_RTF, sy_RTF;
    float sx_LBB, sy_LBB, sx_RBB, sy_RBB, sx_LTB, sy_LTB, sx_RTB, sy_RTB;
    render_project(xL, yB, zF, &sx_LBF, &sy_LBF);
    render_project(xR, yB, zF, &sx_RBF, &sy_RBF);
    render_project(xL, yT, zF, &sx_LTF, &sy_LTF);
    render_project(xR, yT, zF, &sx_RTF, &sy_RTF);
    render_project(xL, yB, zB, &sx_LBB, &sy_LBB);
    render_project(xR, yB, zB, &sx_RBB, &sy_RBB);
    render_project(xL, yT, zB, &sx_LTB, &sy_LTB);
    render_project(xR, yT, zB, &sx_RTB, &sy_RTB);

    uint16_t* const fb_pixels = (uint16_t*)pax_buf_get_pixels(fb);
    bool      const rev_endian = fb->reverse_endianness;
    uint16_t  const front_packed = direct_565_pack(o->front_color,   rev_endian);
    uint16_t  const side_packed  = direct_565_pack(o->side_color,    rev_endian);  // bottom uses side palette
    uint16_t  const top_packed   = direct_565_pack(o->top_color,     rev_endian);
    uint16_t  const out_packed   = direct_565_pack(o->outline_color, rev_endian);

    // Bottom face (visible — camera below). Two triangles covering
    // the underside rectangle (xL..xR, zF..zB) at y=yB.
    direct_565_tri(fb_pixels, sx_LBF, sy_LBF, sx_RBF, sy_RBF, sx_RBB, sy_RBB, side_packed);
    direct_565_tri(fb_pixels, sx_LBF, sy_LBF, sx_RBB, sy_RBB, sx_LBB, sy_LBB, side_packed);

    // Top face — visible only if the camera looks up from below far
    // enough, which it never does at RENDER_CAM_Y = 1 against a
    // span at y ≥ 3. Skip rendering entirely; behind-the-span pixels
    // belong to the sky / mountains, already drawn.

    // Front face (visible while in front of the span).
    if (front_visible) {
        direct_565_tri(fb_pixels, sx_LBF, sy_LBF, sx_RBF, sy_RBF, sx_RTF, sy_RTF, front_packed);
        direct_565_tri(fb_pixels, sx_LBF, sy_LBF, sx_RTF, sy_RTF, sx_LTF, sy_LTF, front_packed);
    }

    (void)top_packed;  // unused for now; kept for future if span top becomes visible

    // Wireframe — the front rectangle + the bottom edges receding
    // toward the back so the span reads as a 3D slab. Cheap (single
    // line each, packed colour cached above).
    if (front_visible) {
        direct_565_line(fb_pixels, (int)sx_LBF, (int)sy_LBF, (int)sx_RBF, (int)sy_RBF, out_packed);
        direct_565_line(fb_pixels, (int)sx_LTF, (int)sy_LTF, (int)sx_RTF, (int)sy_RTF, out_packed);
        direct_565_line(fb_pixels, (int)sx_LBF, (int)sy_LBF, (int)sx_LTF, (int)sy_LTF, out_packed);
        direct_565_line(fb_pixels, (int)sx_RBF, (int)sy_RBF, (int)sx_RTF, (int)sy_RTF, out_packed);
    }
    direct_565_line(fb_pixels, (int)sx_LBF, (int)sy_LBF, (int)sx_LBB, (int)sy_LBB, out_packed);
    direct_565_line(fb_pixels, (int)sx_RBF, (int)sy_RBF, (int)sx_RBB, (int)sy_RBB, out_packed);
    direct_565_line(fb_pixels, (int)sx_LBB, (int)sy_LBB, (int)sx_RBB, (int)sy_RBB, out_packed);
}

// --- Span shadow callback ------------------------------------------
// The span is elevated, so the default "shadow length = height ×
// factor projected from y=0 near face" doesn't model the actual
// shadow geometry. We use (y_base + height) × factor — the top of
// the span as seen from a sun moving down — which produces a long
// shadow as the sun sets and a short one when it's high. Shadow
// shape is a rectangle on the ground spanning the same x range as
// the span, extending toward the camera from the span's near face.
static void span_shadow(pax_buf_t* fb, obstacle_t const* o, float cam_x, float sun_y) {
    (void)cam_x;  // render_project reads the camera global
    if (sun_y >= GAME_SUN_SINK_RANGE_PX) return;

    float const sun_norm  = sun_y / GAME_SUN_SINK_RANGE_PX;
    float const factor    = GAME_SHADOW_LEN_FACTOR_MIN
                          + (GAME_SHADOW_LEN_FACTOR_MAX - GAME_SHADOW_LEN_FACTOR_MIN) * sun_norm;
    float const shadow_h  = BRIDGE_SPAN_Y_BASE + o->height;
    float const shadow_len = shadow_h * factor;

    float const xL = o->x_world - o->half_w;
    float const xR = o->x_world + o->half_w;
    float const z_far = o->z_world - o->half_d;
    if (z_far < RENDER_NEAR_CLIP_Z) return;
    float const z_near_raw = z_far - shadow_len;
    float const z_near = (z_near_raw < RENDER_NEAR_CLIP_Z) ? RENDER_NEAR_CLIP_Z : z_near_raw;

    float sx_NL, sy_NL, sx_NR, sy_NR, sx_FL, sy_FL, sx_FR, sy_FR;
    render_project(xL, 0.0f, z_near, &sx_NL, &sy_NL);
    render_project(xR, 0.0f, z_near, &sx_NR, &sy_NR);
    render_project(xL, 0.0f, z_far,  &sx_FL, &sy_FL);
    render_project(xR, 0.0f, z_far,  &sx_FR, &sy_FR);

    uint16_t* const fb_pixels = (uint16_t*)pax_buf_get_pixels(fb);
    uint16_t  const sh_packed = direct_565_pack(GAME_SHADOW_FLOOR_COLOR, fb->reverse_endianness);
    direct_565_tri(fb_pixels, sx_NL, sy_NL, sx_NR, sy_NR, sx_FR, sy_FR, sh_packed);
    direct_565_tri(fb_pixels, sx_NL, sy_NL, sx_FR, sy_FR, sx_FL, sy_FL, sh_packed);
}

// --- Span collide callback -----------------------------------------
// The ship's collision AABB lives near y=0 (ship altitude ~0.22 u);
// the span sits at y ≥ 3 u, so it physically never touches the ship.
// But the default obstacle collision is x-z only — without this
// override the ship would die on every x-z overlap. IGNORE makes
// the span explicitly non-colliding.
static obstacle_hit_result_t span_collide(obstacle_t* o, struct game_state_s* g, bool came_from_ahead) {
    (void)o; (void)g; (void)came_from_ahead;
    return OBSTACLE_HIT_IGNORE;
}

void bridge_spawn(world_state_t* w, float z) {
    // Pillars sit on top of the side walls, not embedded in them.
    // y_base = WALL_HEIGHT lifts the pillar's bottom face to the
    // wall's top face — the default cube renderer reads y_base now
    // so this is sufficient; no custom draw needed. (Pre-y_base the
    // pillar started at y=0 and the lower 0.67 u overlapped the
    // wall, producing render flicker on the overlap.) Still nudged
    // 0.01 u toward the camera so painter's sort puts them strictly
    // after any co-located wall segment.
    float const pillar_z = z - 0.01f;
    obstacle_t* const left_pillar = obstacle_spawn(
        w, OBSTACLE_KIND_CUBE,
        WALL_X_LEFT, pillar_z,
        WALL_HALF_W, BRIDGE_HALF_D, BRIDGE_PILLAR_HEIGHT,
        CONCRETE_FRONT_COLOR, CONCRETE_SIDE_COLOR,
        CONCRETE_TOP_COLOR,   CONCRETE_OUTLINE_COLOR);
    if (left_pillar) left_pillar->y_base = WALL_HEIGHT;
    obstacle_t* const right_pillar = obstacle_spawn(
        w, OBSTACLE_KIND_CUBE,
        WALL_X_RIGHT, pillar_z,
        WALL_HALF_W, BRIDGE_HALF_D, BRIDGE_PILLAR_HEIGHT,
        CONCRETE_FRONT_COLOR, CONCRETE_SIDE_COLOR,
        CONCRETE_TOP_COLOR,   CONCRETE_OUTLINE_COLOR);
    if (right_pillar) right_pillar->y_base = WALL_HEIGHT;

    // Span: full-width slab with custom draw / shadow / collide so
    // its elevation is rendered correctly and the ship can fly
    // under it.
    obstacle_t* const span = obstacle_spawn(w, OBSTACLE_KIND_CUBE,
                                            0.0f, z,
                                            SPAN_HALF_W, BRIDGE_HALF_D, BRIDGE_SPAN_HEIGHT,
                                            CONCRETE_FRONT_COLOR, CONCRETE_SIDE_COLOR,
                                            CONCRETE_TOP_COLOR,   CONCRETE_OUTLINE_COLOR);
    if (span) {
        span->draw    = span_draw;
        span->shadow  = span_shadow;
        span->collide = span_collide;
    }
}
