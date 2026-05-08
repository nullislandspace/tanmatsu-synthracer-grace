#pragma once

#include "pax_types.h"

// Pre-triangulate all filled polygons used by the synthwave backdrop.
// Must be called once after pax_buf_init and before any synthwave_draw_*
// call. Allocates a few small index arrays in PSRAM that live for the
// lifetime of the program.
void synthwave_init(void);

// Fill the entire framebuffer with the synthwave sky color.
void synthwave_draw_sky(pax_buf_t* fb);

// Draw the five sun bands. `dy` shifts the sun vertically; pass 0 for the
// canonical position, larger positive values to make the sun sink toward
// the horizon. Drawn before the mountains so the silhouette occludes the
// lower half of the sun naturally.
void synthwave_draw_sun(pax_buf_t* fb, float dy);

// Draw the filled mountain silhouette.
void synthwave_draw_mountains(pax_buf_t* fb);

// Draw the cyan wireframe overlay on the mountains.
void synthwave_draw_wireframe(pax_buf_t* fb);

// Draw the magenta horizon line at the top of the floor grid.
void synthwave_draw_top_grid(pax_buf_t* fb);

// Draw one frame of the scrolling grid floor.
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
void synthwave_step(pax_buf_t* fb, float dz_world, float cam_x);
