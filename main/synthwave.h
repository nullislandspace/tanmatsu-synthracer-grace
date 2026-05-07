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

// Draw one frame of the scrolling grid floor. `dj` advances the internal
// scroll counter by that many pixels — pass 1 for normal speed, larger
// values for faster scroll, 0 to freeze.
void synthwave_step(pax_buf_t* fb, int dj);
