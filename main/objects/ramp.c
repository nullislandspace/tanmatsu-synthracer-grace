#include "objects/ramp.h"

#include <stdint.h>

#include "direct_565.h"
#include "render.h"      // render_project, RENDER_NEAR_CLIP_Z
#include "world.h"

// Dull yellow body with a bright-yellow wireframe — distinct from
// the amber gate slabs and the green boosters.
#define RAMP_FRONT_COLOR    0xFFB89820u   // dull yellow (unused face)
#define RAMP_SIDE_COLOR     0xFF7A6515u   // darker — the triangular side
#define RAMP_TOP_COLOR      0xFFB89820u   // dull yellow — the sloped surface
#define RAMP_OUTLINE_COLOR  0xFFFFE84Au   // bright yellow

// Draw callback. The ramp is a triangular prism: a flat bottom at
// world-y 0, a vertical far face, and the sloped top (hypotenuse)
// the ship rides up. Only the two faces the player ever sees are
// drawn — the sloped top and the camera-side triangle; the far
// face and bottom point away from a forward, slightly-raised
// camera.
static void ramp_draw(pax_buf_t* fb, obstacle_t const* o, float cam_x) {
    float const zN_raw = o->z_world - o->half_d;
    float const zF     = o->z_world + o->half_d;
    if (zF < RENDER_NEAR_CLIP_Z) return;
    // Clip the near (low) edge to the near plane so the projection
    // can't blow up once the ramp is right on top of the camera.
    float const zN = (zN_raw < RENDER_NEAR_CLIP_Z) ? RENDER_NEAR_CLIP_Z : zN_raw;

    float const xL   = o->x_world - o->half_w;
    float const xR   = o->x_world + o->half_w;
    float const rise = o->height;

    // 6 corners: near-bottom, far-bottom, far-top — each L and R.
    float sx_nbL, sy_nbL, sx_nbR, sy_nbR;
    float sx_fbL, sy_fbL, sx_fbR, sy_fbR;
    float sx_ftL, sy_ftL, sx_ftR, sy_ftR;
    render_project(xL, 0.0f, zN, &sx_nbL, &sy_nbL);
    render_project(xR, 0.0f, zN, &sx_nbR, &sy_nbR);
    render_project(xL, 0.0f, zF, &sx_fbL, &sy_fbL);
    render_project(xR, 0.0f, zF, &sx_fbR, &sy_fbR);
    render_project(xL, rise, zF, &sx_ftL, &sy_ftL);
    render_project(xR, rise, zF, &sx_ftR, &sy_ftR);

    uint16_t* const fb_pixels = (uint16_t*)pax_buf_get_pixels(fb);
    bool      const rev       = fb->reverse_endianness;
    uint16_t  const top_pk    = direct_565_pack(o->top_color,     rev);
    uint16_t  const side_pk   = direct_565_pack(o->side_color,    rev);
    uint16_t  const out_pk     = direct_565_pack(o->outline_color, rev);
    // A side face is visible only when the camera is beyond that
    // edge of the ramp — when the camera is *over* the ramp
    // (xL <= cam_x <= xR), as it is whenever the ship rides it,
    // neither side shows. (Comparing against the centre, not the
    // edges, always drew one face — that was the render bug.)
    bool      const show_left  = (cam_x < xL);
    bool      const show_right = (cam_x > xR);

    // Camera-side triangle (the wedge's profile) first, then the
    // sloped top overpaints any sliver bleeding through.
    if (show_left) {
        direct_565_tri(fb_pixels, sx_nbL, sy_nbL, sx_fbL, sy_fbL, sx_ftL, sy_ftL, side_pk);
    } else if (show_right) {
        direct_565_tri(fb_pixels, sx_nbR, sy_nbR, sx_fbR, sy_fbR, sx_ftR, sy_ftR, side_pk);
    }
    // Sloped top — quad nbL → nbR → ftR → ftL, two triangles.
    direct_565_tri(fb_pixels, sx_nbL, sy_nbL, sx_nbR, sy_nbR, sx_ftR, sy_ftR, top_pk);
    direct_565_tri(fb_pixels, sx_nbL, sy_nbL, sx_ftR, sy_ftR, sx_ftL, sy_ftL, top_pk);

    // Bright outline — the sloped top's four edges, plus the two
    // sloping edges of the visible side triangle.
    direct_565_line(fb_pixels, (int)sx_nbL, (int)sy_nbL, (int)sx_nbR, (int)sy_nbR, out_pk);
    direct_565_line(fb_pixels, (int)sx_nbR, (int)sy_nbR, (int)sx_ftR, (int)sy_ftR, out_pk);
    direct_565_line(fb_pixels, (int)sx_ftR, (int)sy_ftR, (int)sx_ftL, (int)sy_ftL, out_pk);
    direct_565_line(fb_pixels, (int)sx_ftL, (int)sy_ftL, (int)sx_nbL, (int)sy_nbL, out_pk);
    if (show_left) {
        direct_565_line(fb_pixels, (int)sx_nbL, (int)sy_nbL, (int)sx_fbL, (int)sy_fbL, out_pk);
        direct_565_line(fb_pixels, (int)sx_fbL, (int)sy_fbL, (int)sx_ftL, (int)sy_ftL, out_pk);
    } else if (show_right) {
        direct_565_line(fb_pixels, (int)sx_nbR, (int)sy_nbR, (int)sx_fbR, (int)sy_fbR, out_pk);
        direct_565_line(fb_pixels, (int)sx_fbR, (int)sy_fbR, (int)sx_ftR, (int)sy_ftR, out_pk);
    }
}

obstacle_t* ramp_spawn_at(world_state_t* w, float x, float z,
                          float half_w, float half_d, float rise) {
    obstacle_t* const o = obstacle_spawn(
        w, OBSTACLE_KIND_RAMP,
        x, z,
        half_w, half_d, rise,
        RAMP_FRONT_COLOR, RAMP_SIDE_COLOR, RAMP_TOP_COLOR, RAMP_OUTLINE_COLOR);
    if (o) o->draw = ramp_draw;
    return o;
}
