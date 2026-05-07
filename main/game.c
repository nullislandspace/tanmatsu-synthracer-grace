#include "game.h"

#include "pax_gfx.h"
#include "shapes/pax_tris.h"

// Tunables. Hand-picked for "responsive but not twitchy" feel; will be
// re-tuned once obstacles ship in Phase 3 and steering needs to feel
// committed enough to make dodges deliberate.
#define SHIP_X_MIN        40.0f
#define SHIP_X_MAX        760.0f
#define SHIP_ACCEL        2400.0f  // px/s² applied while a steer key is held
#define SHIP_MAX_VX       450.0f   // px/s lateral cap
#define SHIP_FRICTION     6.0f     // exponential decay rate (1/s)

// Ship body sits a fixed height above the bottom of the grid floor. The
// 3D scene from Phase 3 will project obstacles to this same baseline so
// near-camera collisions look physically plausible.
#define SHIP_Y            430.0f

// Synthwave palette
#define SHIP_BODY_COLOR   0xFFFFFF6Bu  // sun-yellow main fuselage
#define SHIP_WING_COLOR   0xFFF71FF1u  // grid-magenta wings

void game_init(game_state_t* g) {
    g->ship_x  = 400.0f;
    g->ship_vx = 0.0f;
}

void game_step(game_state_t* g, float dt, int steer) {
    if (dt <= 0.0f) return;

    // Apply acceleration from steering, plus exponential friction so the
    // ship coasts to a halt when the player releases keys. Using
    // exp-style friction (multiply by exp(-k*dt)) instead of a flat
    // multiplier keeps the feel framerate-independent.
    g->ship_vx += (float)steer * SHIP_ACCEL * dt;

    float decay = 1.0f - SHIP_FRICTION * dt;
    if (decay < 0.0f) decay = 0.0f;
    g->ship_vx *= decay;

    if (g->ship_vx >  SHIP_MAX_VX) g->ship_vx =  SHIP_MAX_VX;
    if (g->ship_vx < -SHIP_MAX_VX) g->ship_vx = -SHIP_MAX_VX;

    g->ship_x += g->ship_vx * dt;

    if (g->ship_x < SHIP_X_MIN) {
        g->ship_x  = SHIP_X_MIN;
        g->ship_vx = 0.0f;
    } else if (g->ship_x > SHIP_X_MAX) {
        g->ship_x  = SHIP_X_MAX;
        g->ship_vx = 0.0f;
    }
}

void game_draw_ship(pax_buf_t* fb, game_state_t const* g) {
    float cx  = g->ship_x;
    float top = SHIP_Y - 15.0f;
    float bot = SHIP_Y + 15.0f;

    // Center fuselage — narrow delta pointing up the track.
    pax_simple_tri(fb, SHIP_BODY_COLOR, cx, top, cx - 10.0f, bot, cx + 10.0f, bot);

    // Left wing
    pax_simple_tri(fb, SHIP_WING_COLOR, cx - 10.0f, SHIP_Y + 5.0f, cx - 30.0f, bot, cx - 10.0f, bot);

    // Right wing
    pax_simple_tri(fb, SHIP_WING_COLOR, cx + 10.0f, SHIP_Y + 5.0f, cx + 30.0f, bot, cx + 10.0f, bot);
}
