#pragma once

#include <stdbool.h>

#include "pax_types.h"

// Pre-triangulate all filled polygons used by the synthwave backdrop.
// Must be called once after pax_buf_init and before any synthwave_draw_*
// call. Allocates a few small index arrays in PSRAM that live for the
// lifetime of the program.
void synthwave_init(void);

// Fill the entire framebuffer with the synthwave sky color.
void synthwave_draw_sky(pax_buf_t* fb);

// Draw the five sun bands. `dx` / `dy` shift the sun; pass 0/0 for the
// canonical screen position. `dx` is used to render the sun into a tight
// cache buffer (pass -SUN_CACHE_LOG_X so the disc's left edge lands near
// cache x=0); `dy` makes the sun sink toward the horizon. Drawn before the
// mountains so the silhouette occludes the lower half of the sun naturally.
void synthwave_draw_sun(pax_buf_t* fb, float dx, float dy);

// Draw the filled mountain silhouette. `y_bias` shifts every vertex by
// that many pixels — pass 0 for the canonical position, a negative
// value to render into a tighter cache buffer whose top row maps to
// the mountain band's top in the final framebuffer.
void synthwave_draw_mountains(pax_buf_t* fb, float y_bias);

// Draw the cyan wireframe overlay on the mountains. `y_bias` follows
// the same convention as `synthwave_draw_mountains`.
void synthwave_draw_wireframe(pax_buf_t* fb, float y_bias);

// Draw the magenta horizon line at the top of the floor grid.
// `y_bias` shifts the line vertically — same convention as the
// mountain functions, so the horizon stays aligned when rendering
// into a cache buffer with a non-zero top offset.
void synthwave_draw_top_grid(pax_buf_t* fb, float y_bias);

// (The floor base rect moved to the engine PPA floor FILL —
// backdrop_submit_fill_floor() — so it can run on the PPA alongside the
// sky FILL during the geometry-prepare instead of as CPU work. The shadow
// pass and these grid lines still paint on top of that filled base.)

// Animated grid lines on top of whatever the floor pass has
// already painted (floor base + obstacle shadows). Drawn last so
// the magenta lane lines and horizontal stripes are visible in
// shadowed regions too.
//
// `dz_world` is the world-z distance the camera advanced this frame
// (typically `ship_speed_z * dt`). Both the horizontal scanlines and
// the vertical lane lines are projected with the same pinhole math
// as render_project, so the floor's apparent motion matches the
// motion of obstacle bases — a unit step in world-z moves a floor
// stripe by exactly the same screen-y delta as it moves an
// obstacle's base at that z.
//
// `cam_x` is the camera's lateral world-x position. Vertical lane
// lines are drawn in world space (anchored to the world, not the
// screen) so they pan correctly as the ship moves laterally.
//
// `cam_y` is the camera's height. The floor projection scales with
// it exactly as render_project does (sy = horizon + F*cam_y/z), so
// the grid rises and sinks in lockstep with the obstacles sitting
// on it when the camera follows a jump.
void synthwave_step_lines(pax_buf_t* fb, float dz_world, float cam_x, float cam_y);
