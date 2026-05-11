#pragma once

#include "magicnumbers.h"
#include "pax_gfx.h"
#include "world.h"

// Pinhole-camera projection parameters. The horizon-y matches
// synthwave's GRID_HORIZON_Y_BASE - GRID_LIFT_PX (= 256), so projected
// world geometry vanishes correctly into the synthwave horizon.
#define RENDER_HALF_W       ((float)DISPLAY_LOG_W / 2.0f)
#define RENDER_HORIZON_Y    256.0f
#define RENDER_FOCAL_LEN    450.0f
#define RENDER_CAM_Y        1.0f

// Project a world point (x_w, y_w, z_w) onto the screen. y=0 is the
// ground plane, +y is up; +z is forward (away from the camera).
// `cam_x` is the camera's lateral position in world units — the camera
// follows the ship so the world pans around it. Out values are in pax
// logical pixels.
void render_project(float x_w, float y_w, float z_w, float cam_x, float* out_sx, float* out_sy);

// Draw shadow quads on the floor for every active cube obstacle.
// Each shadow is a flat trapezoid on the y=0 ground plane,
// projecting from the obstacle's near face toward the camera by
// `obstacle.height * factor`, where `factor` is the linear
// interpolation between `GAME_SHADOW_LEN_FACTOR_MIN` and
// `GAME_SHADOW_LEN_FACTOR_MAX` keyed on `sun_y / SINK_RANGE`.
//
// Drawn between the floor paint (`synthwave_step`) and the
// obstacles themselves so the cube tops overpaint any shadow
// geometry directly under them. When the sun has fully set the
// floor base is already the shadow colour (see synthwave_step's
// `fully_shadowed` flag), and this function does nothing.
void render_shadows(pax_buf_t* fb, world_state_t const* w, float cam_x, float sun_y);

// Draw all active obstacles. Sorted back-to-front (descending z) and
// drawn as flat-shaded front faces with painter's algorithm — no
// z-buffer needed. `cam_x` is the camera's lateral world position.
void render_obstacles(pax_buf_t* fb, world_state_t const* w, float cam_x);
