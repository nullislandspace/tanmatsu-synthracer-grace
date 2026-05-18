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

// --- Span rendering ------------------------------------------------
// The span sits at y in [BRIDGE_SPAN_Y_BASE, BRIDGE_SPAN_Y_BASE +
// BRIDGE_SPAN_HEIGHT]. Its obstacle carries that elevation in
// `y_base` (set in bridge_spawn), and the default cube emitter is
// y_base-aware, so the span needs no custom emit callback — the
// generic emitter renders the underside (the face the player flies
// beneath), front and any visible side correctly, and the depth
// buffer occludes it against everything else.

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

// (Phase 9.1b retired the span_collide IGNORE hack. Collision is
// now Y-aware, so the elevated span — sitting at y ≥ 3 u — simply
// never Y-overlaps the ground-level ship and the default cube
// collision dispatch handles it correctly. The span is now also a
// landable surface like any other obstacle top.)

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

    // Span: full-width slab. Rendering is the default y_base-aware
    // cube emitter; only the elevated shadow needs a custom callback.
    // Collision is the default Y-aware cube dispatch — the span sits
    // well above the ship, so the ship flies under it (and, post-9.1c,
    // can land on it).
    obstacle_t* const span = obstacle_spawn(w, OBSTACLE_KIND_CUBE,
                                            0.0f, z,
                                            SPAN_HALF_W, BRIDGE_HALF_D, BRIDGE_SPAN_HEIGHT,
                                            CONCRETE_FRONT_COLOR, CONCRETE_SIDE_COLOR,
                                            CONCRETE_TOP_COLOR,   CONCRETE_OUTLINE_COLOR);
    if (span) {
        // y_base carries the span's elevation: the emitter renders it
        // there, the Y-aware collision AABB sits there, and the ship
        // flies under (and can land on) it. Pre-9.1b a span_collide
        // IGNORE hack masked a y_base of 0; that is long gone.
        span->y_base = BRIDGE_SPAN_Y_BASE;
        span->shadow = span_shadow;
    }
}
