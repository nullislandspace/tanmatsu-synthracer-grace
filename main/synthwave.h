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

// Draw the five sun bands. `dy` shifts the sun vertically; pass 0 for the
// canonical position, larger positive values to make the sun sink toward
// the horizon. Drawn before the mountains so the silhouette occludes the
// lower half of the sun naturally.
void synthwave_draw_sun(pax_buf_t* fb, float dy);

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

// Floor base color — a solid rectangle covering the below-horizon
// region of the framebuffer. Called *before* the shadow pass so
// shadows can paint on top of the base without lane lines being
// erased; the lane lines + stripes come on top of both via
// `synthwave_step_lines`.
//
// `fully_shadowed` darkens the floor base to GAME_SHADOW_FLOOR_COLOR
// when the sun has fully set (the whole world is in shadow). The
// magenta lane lines and stripes still render on top — they remain
// visible against the darker base, matching the synthwave aesthetic
// where pink/cyan lines cut through deep purple.
void synthwave_step_base(pax_buf_t* fb, bool fully_shadowed);

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
void synthwave_step_lines(pax_buf_t* fb, float dz_world, float cam_x);
