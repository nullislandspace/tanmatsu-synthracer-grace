#include "objects/ramp.h"

#include <stdint.h>

#include "render.h"      // render_camera, RENDER_NEAR_CLIP_Z
#include "se_scene.h"       // scene_tri, scene_line
#include "world.h"

// Dull yellow body with a bright-yellow wireframe — distinct from
// the amber gate slabs and the green boosters.
#define RAMP_FRONT_COLOR    0xFFB89820u   // dull yellow (unused face)
#define RAMP_SIDE_COLOR     0xFF7A6515u   // darker — the triangular side
#define RAMP_TOP_COLOR      0xFFB89820u   // dull yellow — the sloped surface
#define RAMP_OUTLINE_COLOR  0xFFFFE84Au   // bright yellow

// Emit callback. The ramp is a triangular prism: a flat bottom at
// world-y 0, a vertical far face, and the sloped top (hypotenuse)
// the ship rides up. Only the two faces the player ever sees are
// emitted — the sloped top and the camera-side triangle; the far
// face and bottom point away from a forward, slightly-raised
// camera. The depth buffer handles occlusion against everything
// else, so no draw-order reasoning is needed.
static void ramp_emit(obstacle_t const* o) {
    float const zN = o->z_world - o->half_d;   // near edge (low)
    float const zF = o->z_world + o->half_d;   // far edge (top of slope)
    if (zF < RENDER_NEAR_CLIP_Z) return;

    float const xL   = o->x_world - o->half_w;
    float const xR   = o->x_world + o->half_w;
    float const rise = o->height;

    uint32_t const top  = o->top_color;
    uint32_t const side = o->side_color;
    uint32_t const out  = o->outline_color;

    // A side face is visible only when the camera is beyond that
    // edge of the ramp; while the ship rides the ramp the camera is
    // over it (xL <= cam.x <= xR) and neither side shows.
    render_camera_t const cam = render_camera();
    bool const show_left  = (cam.x < xL);
    bool const show_right = (cam.x > xR);

    // Camera-side triangle (the wedge's profile).
    if (show_left) {
        scene_tri(xL, 0.0f, zN,  xL, 0.0f, zF,  xL, rise, zF,  side);
    } else if (show_right) {
        scene_tri(xR, 0.0f, zN,  xR, 0.0f, zF,  xR, rise, zF,  side);
    }
    // Sloped top — quad nbL → nbR → ftR → ftL, two triangles.
    scene_tri(xL, 0.0f, zN,  xR, 0.0f, zN,  xR, rise, zF,  top);
    scene_tri(xL, 0.0f, zN,  xR, rise, zF,  xL, rise, zF,  top);

    // Bright outline — the sloped top's four edges, plus the two
    // sloping edges of the visible side triangle.
    scene_line(xL, 0.0f, zN,  xR, 0.0f, zN,  out);
    scene_line(xR, 0.0f, zN,  xR, rise, zF,  out);
    scene_line(xR, rise, zF,  xL, rise, zF,  out);
    scene_line(xL, rise, zF,  xL, 0.0f, zN,  out);
    if (show_left) {
        scene_line(xL, 0.0f, zN,  xL, 0.0f, zF,  out);
        scene_line(xL, 0.0f, zF,  xL, rise, zF,  out);
    } else if (show_right) {
        scene_line(xR, 0.0f, zN,  xR, 0.0f, zF,  out);
        scene_line(xR, 0.0f, zF,  xR, rise, zF,  out);
    }
}

obstacle_t* ramp_spawn_at(world_state_t* w, float x, float z,
                          float half_w, float half_d, float rise) {
    obstacle_t* const o = obstacle_spawn(
        w, OBSTACLE_KIND_RAMP,
        x, z,
        half_w, half_d, rise,
        RAMP_FRONT_COLOR, RAMP_SIDE_COLOR, RAMP_TOP_COLOR, RAMP_OUTLINE_COLOR);
    if (o) o->emit = ramp_emit;
    return o;
}
