#pragma once

#include <stdbool.h>

#include "pax_gfx.h"

// Phase 2 minimum: just the ship's lateral position and velocity. Will
// grow to include score, multiplier, sun timer, region progress, pickup
// inventory, etc. in later phases.
typedef struct {
    float ship_x;   // lateral, in screen-space pixels (0..800)
    float ship_vx;  // velocity along x in px/s
} game_state_t;

// Reset the run.
void game_init(game_state_t* g);

// Advance the simulation by `dt` seconds. `steer` is one of {-1, 0, +1}.
void game_step(game_state_t* g, float dt, int steer);

// Render the ship. Screen-space, no projection.
void game_draw_ship(pax_buf_t* fb, game_state_t const* g);
