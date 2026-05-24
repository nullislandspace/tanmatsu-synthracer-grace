#pragma once

#include "magicnumbers.h"
#include "pax_gfx.h"
#include "se_scene.h"   // engine 3D scene: scene_*, camera + projection,
                        // and (via se_config.h) the RENDER_* constants
#include "world.h"

// Game-side world rendering. The 3D pipeline itself — the z-buffer
// rasterizer, the pinhole camera (render_camera_t / render_set_camera /
// render_camera), render_project, and the RENDER_* projection constants —
// now lives in the engine (se_scene.h / se_config.h) and is re-exported
// above so existing call sites keep working. What remains here is the
// game-specific glue that walks the obstacle pool: submitting its
// geometry to the engine scene and drawing the obstacles' floor shadows.

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

// Emit every active obstacle's geometry into the scene (see scene.h).
// No sort and no painter's algorithm — the scene's per-pixel z-buffer
// resolves visibility, so obstacles are submitted in pool order. The
// caller must have called scene_begin() first and must call
// scene_flush() after all geometry (obstacles + ship) is submitted.
// The camera is read from render_camera().
void render_submit_obstacles(world_state_t const* w);
