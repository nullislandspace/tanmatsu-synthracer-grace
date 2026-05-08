#include "game.h"

#include "pax_gfx.h"
#include "render.h"
#include "shapes/pax_tris.h"

// Lateral physics, in world units now (ship.x in [-5, +5]). Constants
// re-tuned from the screen-space versions: 2400 px/s² ÷ 174 px/u
// (visible width per world unit at ship-z) ≈ 14 u/s², rounded to 16
// for slightly snappier feel. 450 px/s max → ~2.6 u/s, rounded to 3.
#define SHIP_ACCEL_WORLD      16.0f
#define SHIP_MAX_VX_WORLD     3.0f
#define SHIP_FRICTION         6.0f
#define SHIP_X_MIN_WORLD     -5.0f
#define SHIP_X_MAX_WORLD      5.0f

// Ship draw size in screen pixels (ship is locked to screen centre by
// the camera-follow, so its size doesn't perspective-scale with z).
#define SHIP_DRAW_HALF_W     30.0f
#define SHIP_DRAW_HALF_H     15.0f

// Synthwave palette
#define SHIP_BODY_COLOR   0xFFFFFF6Bu  // sun-yellow main fuselage
#define SHIP_WING_COLOR   0xFFF71FF1u  // grid-magenta wings

void game_init(game_state_t* g) {
    g->ship_x_world  = 0.0f;
    g->ship_vx_world = 0.0f;
    g->ship_speed_z  = SHIP_BASE_SPEED_Z;
    g->cam_x         = 0.0f;
}

void game_step(game_state_t* g, float dt, int steer) {
    if (dt <= 0.0f) return;

    g->ship_vx_world += (float)steer * SHIP_ACCEL_WORLD * dt;

    // Exp-style friction so the feel is framerate-independent and the
    // ship coasts to a halt when steering is released.
    float decay = 1.0f - SHIP_FRICTION * dt;
    if (decay < 0.0f) decay = 0.0f;
    g->ship_vx_world *= decay;

    if (g->ship_vx_world >  SHIP_MAX_VX_WORLD) g->ship_vx_world =  SHIP_MAX_VX_WORLD;
    if (g->ship_vx_world < -SHIP_MAX_VX_WORLD) g->ship_vx_world = -SHIP_MAX_VX_WORLD;

    g->ship_x_world += g->ship_vx_world * dt;

    if (g->ship_x_world < SHIP_X_MIN_WORLD) {
        g->ship_x_world  = SHIP_X_MIN_WORLD;
        g->ship_vx_world = 0.0f;
    } else if (g->ship_x_world > SHIP_X_MAX_WORLD) {
        g->ship_x_world  = SHIP_X_MAX_WORLD;
        g->ship_vx_world = 0.0f;
    }

    // Camera locked to the ship laterally — the ship stays centred on
    // screen and the world pans around it. A small follow-lag could be
    // added later if the centred-ship feel reads as too rigid.
    g->cam_x = g->ship_x_world;
}

void game_draw_ship(pax_buf_t* fb, game_state_t const* g) {
    // With cam_x = ship_x_world, the ship projects exactly to screen
    // centre. The Y is computed from the ship's z plane on the ground.
    float sx, sy_base;
    render_project(g->ship_x_world, 0.0f, SHIP_Z_PLANE, g->cam_x, &sx, &sy_base);

    // Anchor the ship's tail (sy_base) at the ground, body extends up.
    float cy  = sy_base - SHIP_DRAW_HALF_H;
    float top = cy - SHIP_DRAW_HALF_H;
    float bot = cy + SHIP_DRAW_HALF_H;

    pax_simple_tri(fb, SHIP_BODY_COLOR, sx, top, sx - 10.0f, bot, sx + 10.0f, bot);
    pax_simple_tri(fb, SHIP_WING_COLOR, sx - 10.0f, cy + 5.0f, sx - SHIP_DRAW_HALF_W, bot, sx - 10.0f, bot);
    pax_simple_tri(fb, SHIP_WING_COLOR, sx + 10.0f, cy + 5.0f, sx + SHIP_DRAW_HALF_W, bot, sx + 10.0f, bot);
}
