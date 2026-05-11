#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pax_gfx.h"
#include "world.h"

// Phase 4: ship state plus collision flags and a small spark pool
// for the side-scrape visual effect. Phase 5 will reuse the
// scrape decay/recovery mechanic for the sun-shadow slowdown.
#define SHIP_Z_PLANE      2.0f
#define SHIP_BASE_Y       0.22f
#define SHIP_BASE_SPEED_Z 12.0f

// Ship collision AABB. The tetrahedron mesh runs from local
// z = -0.36 (tail) to +0.32 (nose), so the AABB is centred at
// SHIP_Z_PLANE + (-0.36 + 0.32)/2 = 1.98 with half-depth 0.34.
#define SHIP_COLLISION_HALF_W 0.28f
#define SHIP_COLLISION_HALF_D 0.34f
#define SHIP_COLLISION_Z_C    1.98f

// Playfield lateral bounds. Ship can't move past these; obstacles
// placed entirely outside this range are "boundary" obstacles
// (side walls) and collide as scrape-only because the ship's
// centre can't enter their x extent.
#define SHIP_X_MIN_WORLD     (-5.0f)
#define SHIP_X_MAX_WORLD      (5.0f)

typedef struct {
    float ship_x_world;       // lateral, world units. Track is [-5, +5].
    float ship_speed_z;       // current forward velocity. Drifts toward target.
    float ship_base_speed_z;  // unhindered target speed. Adjustable via debug knob.
    float bank;               // -1..+1 signed banking factor
    float cam_x;              // camera follows ship laterally

    // Set by game_collide each frame. game_after_collide reads
    // these to pick the speed target; game_draw_sparks reads them
    // to emit the wingtip burst.
    bool  scrape_left;
    bool  scrape_right;
} game_state_t;

// Reset the run (zeroes the spark pool too).
void game_init(game_state_t* g);

// Apply bank dynamics and lateral motion for this frame. Call this
// *before* game_collide so the position passed to collision is the
// position the player is trying to be in this frame.
void game_step(game_state_t* g, float dt, int steer);

// AABB collision pass against every active entry in the obstacle
// pool. Classifies each overlap by axis of smallest penetration:
//   * smallest on x → side scrape (sets scrape_left/scrape_right
//     and pushes the ship out by `x_pen` so it physically can't
//     penetrate the wall);
//   * smallest on z AND obs.z_world ahead of ship → head-on
//     (returns true; caller flips the app state to GAME_OVER).
// Smallest-on-z when the obstacle has already drifted past the
// ship's z centre is still treated as a scrape — it's just a long
// wall segment whose back edge has crept past the ship.
bool game_collide(game_state_t* g, world_state_t const* w);

// Speed dynamics + spark emission/advance. Reads scrape_left/
// scrape_right (set by game_collide) to ramp ship_speed_z toward
// the appropriate target and emit sparks from the wing tips on the
// scraping side(s). Call after game_collide so the speed and the
// emitted sparks reflect this frame's contact state.
void game_after_collide(game_state_t* g, float dt);

// Render the ship as a 3D mesh, banked by `g->bank`.
void game_draw_ship(pax_buf_t* fb, game_state_t const* g);

// Draw a radial burst of red lines from each scraping wingtip.
// No persistence — each call paints a fresh random pattern, so
// the effect is purely a per-frame indication of "this wingtip is
// in contact right now". Only meaningful during PLAYING; callers
// in other states should skip it.
void game_draw_sparks(pax_buf_t* fb, game_state_t const* g);
