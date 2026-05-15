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
#define RENDER_CAM_Y        1.0f   // camera's resting (grounded) height

// Near-plane z below which projections blow up. Shared by render.c
// and any custom-draw object modules that need to clip their own
// geometry the same way the default cube renderer does.
#define RENDER_NEAR_CLIP_Z  0.5f

// The camera. A single render-module global, set once per frame via
// render_set_camera() and read by render_project. `x` follows the
// ship laterally; `y` follows it vertically (a partial follow of the
// ship's jump altitude — see main.c). Kept as a struct so it can
// gain fields later (zoom, shake, look-ahead) without touching call
// sites. cam_y lives ONLY here — it is never threaded as a parameter.
typedef struct {
    float x;
    float y;
} render_camera_t;

// Set / read the camera. render_set_camera is called once per frame
// before the scene is drawn.
void            render_set_camera(float x, float y);
render_camera_t render_camera(void);

// Project a world point (x_w, y_w, z_w) onto the screen. y=0 is the
// ground plane, +y is up; +z is forward (away from the camera). The
// camera position is read from the render-module global. Out values
// are in pax logical pixels.
void render_project(float x_w, float y_w, float z_w, float* out_sx, float* out_sy);

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
