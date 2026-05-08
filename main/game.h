#pragma once

#include <stdbool.h>

#include "pax_gfx.h"

// Phase 3: lateral pose + a constant forward speed. Phase 5 will
// modulate ship_speed_z based on the sun timer, shadow state, and boost
// pickups; for now it stays at a fixed base value.
typedef struct {
    float ship_x_world;  // lateral, world units. Track is [-5, +5].
    float ship_vx_world; // lateral velocity in world u/s
    float ship_speed_z;  // forward velocity in world units / s
    float cam_x;         // camera follows the ship laterally so the
                         // ship stays centred on screen and the world
                         // pans around it.
} game_state_t;

// Ship's z-plane in world coords. Used for projection (ship draw) and
// collision (Phase 4). The camera sits at z=0; the ship is a couple of
// units in front so obstacles approach visibly before crossing it.
#define SHIP_Z_PLANE     2.5f

// Base forward speed in world units / s. Exposed so render/world code
// can scale visual effects (e.g. floor scroll cadence) against it.
// Tuned for the post-playfield-rework feel: time from spawn (z=100) to
// ship plane (z≈2.5) is ~8 s, with reaction time of ~1.5 s once an
// obstacle becomes visually distinguishable around z=20.
#define SHIP_BASE_SPEED_Z 12.0f

// Reset the run.
void game_init(game_state_t* g);

// Advance the simulation by `dt` seconds. `steer` is one of {-1, 0, +1}.
void game_step(game_state_t* g, float dt, int steer);

// Render the ship. Screen-space, no projection.
void game_draw_ship(pax_buf_t* fb, game_state_t const* g);
