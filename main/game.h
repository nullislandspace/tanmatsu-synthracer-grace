#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "magicnumbers.h"
#include "pax_gfx.h"
#include "world.h"

// Phase 4: ship state plus collision flags and a small spark pool
// for the side-scrape visual effect. Phase 5 will reuse the
// scrape decay/recovery mechanic for the sun-shadow slowdown.
#define SHIP_Z_PLANE      2.0f
#define SHIP_BASE_Y       0.22f
// Ship cruise speed (world units / s). Driven from magicnumbers.h
// so the world generator's stage/rest distance budgets and the
// game's default speed stay in sync.
#define SHIP_BASE_SPEED_Z GAMEPLAY_CRUISE_SPEED

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

// Speed-booster state machine. Boost transitions:
//   IDLE → RAMPING (on pickup)
//   RAMPING → HOLDING (after RAMP_UP_SECONDS)
//   HOLDING → COASTING (after HOLD_SECONDS)
//   COASTING → IDLE (when ship_speed reaches base, or speed is
//                    decreased below base by shadow stall)
// Picking up another booster while non-IDLE restarts at RAMPING
// with the current speed as ramp start.
typedef enum {
    BOOST_IDLE = 0,
    BOOST_RAMPING,
    BOOST_HOLDING,
    BOOST_COASTING,
} boost_phase_t;

// Crash explosion: a fixed pool of screen-space spark particles.
// On a head-on crash the ship is replaced by a burst of these,
// animated until they burn out (≈ the crash SFX length). Pure
// visual ephemera — spawned by game_crash_burst, integrated by
// game_crash_tick, drawn by game_draw_crash_sparks.
#define CRASH_SPARK_COUNT 56

typedef struct {
    float x, y;        // screen-space position, pixels
    float vx, vy;      // screen-space velocity, pixels/s
    float life;        // life remaining, seconds (<= 0 ⇒ dead)
    float life_max;    // initial life, for the fade ratio
} crash_spark_t;

// game_state_t is referred to by forward declaration in save.h to
// avoid a transitive include — keep the typedef name `game_state_t`
// stable (the tag `game_state_s` is what save.h forward-declares).
typedef struct game_state_s {
    float ship_x_world;       // lateral, world units. Track is [-5, +5].
    float ship_speed_z;       // current forward velocity. Drifts toward target.
    float ship_base_speed_z;  // unhindered target speed. Adjustable via debug knob.
    float bank;               // -1..+1 signed banking factor
    float cam_x;              // camera follows ship laterally

    // Vertical motion (Phase 9.1). `ship_y` is altitude above the
    // ship's rest height (SHIP_BASE_Y); 0 = grounded on the floor.
    // `ship_vy` is vertical velocity — a jump injects it, game_step
    // integrates gravity. The camera does not follow Y, so the ship
    // visibly rises in frame.
    float ship_y;
    float ship_vy;

    // Phase 6 scoring. `multiplier` defaults to 1 and is bumped by
    // Tri pickups (not yet wired). `multiplier_max` tracks the peak
    // value reached this run for the per-run stat. Score and
    // distance accumulate per-frame in game_after_collide as
    // `dz = ship_speed_z * dt`. score is `Σ dz * multiplier`.
    double distance_traveled; // world units travelled by the ship this run
    double score;             // accumulated score (distance × current multiplier)
    int    multiplier;        // current multiplier (1× default)
    int    multiplier_max;    // peak multiplier reached this run

    // Per-run pickup counters. Incremented in game_collide on
    // contact; copied verbatim into save_data.stats.last_run when
    // the run ends so the player's career totals reflect what was
    // collected. Only speed_boost is wired today; the others are
    // placeholders for Phases 6 and 9.
    int pickups_speed_boost;
    int pickups_tri;
    int pickups_jump;
    int pickups_shield;

    // Phase 5: sun position drives the run length. 0 = sun high
    // (start of run); GAME_SUN_SINK_RANGE_PX = fully behind the
    // mountains. Driven each frame by `sun_y += dt × rate`, where
    // rate depends on ship speed: at cruise → base sink rate;
    // slower → faster sun; faster → slower / negative (sun rises
    // back up).
    float sun_y;

    // Set each frame by game_step. True if any obstacle currently
    // shadows the ship, OR the sun is fully set. Drives the speed
    // decel and a darker ship sprite tint.
    bool  in_shadow;

    // Set by game_collide each frame. game_after_collide reads
    // these to pick the speed target; game_draw_sparks reads them
    // to emit the wingtip burst.
    bool  scrape_left;
    bool  scrape_right;

    // Active speed-booster phase + the timer for the current
    // phase + the ship's speed at the start of the RAMPING phase
    // (so the lerp interpolates from there to the boost target).
    boost_phase_t boost_phase;
    float         boost_phase_time;
    float         boost_ramp_start_speed;

    // Edge-trigger flag for audio: set true inside `game_collide`
    // on the same frame a booster is consumed; main.c reads + clears
    // it once per frame to fire `sfx_pickup_ding_play()`. Lives on
    // game_state so the audio side stays out of game logic.
    bool  just_picked_up_booster;

    // Same pattern for the Tri pickup (Phase 6). `tri_pickup_slot`
    // is the slot index 0..4 within the current multiplier cycle —
    // used to pick the plink SFX's pitch (C5/D5/E5/G5/A5). Set to
    // `(pickups_tri - 1) % 5` after the increment; ignored when
    // `just_picked_up_tri` is false.
    bool  just_picked_up_tri;
    int   tri_pickup_slot;

    // Crash explosion spark pool (see crash_spark_t above). Zeroed
    // by game_init; filled by game_crash_burst on the head-on crash
    // frame, then aged each frame by game_crash_tick.
    crash_spark_t crash_sparks[CRASH_SPARK_COUNT];
} game_state_t;

// Reset the run (zeroes the spark pool too).
void game_init(game_state_t* g);

// Apply bank dynamics and lateral motion for this frame. Call this
// *before* game_collide so the position passed to collision is the
// position the player is trying to be in this frame. `steer` is a
// signed deflection in [-1.0, +1.0] (keys give ±1, gyro tilt is
// proportional).
void game_step(game_state_t* g, float dt, float steer);

// Trigger a jump: inject GAME_JUMP_SPEED into `ship_vy` if the ship
// is grounded. A no-op while airborne (no double-jump). game_step's
// vertical integration carries the resulting arc. Phase 9.1f will
// gate this on the jump-pickup inventory; until then it is free.
void game_jump(game_state_t* g);

// Swept AABB collision pass against every active entry in the
// obstacle pool. The z range tested is the obstacle's *swept*
// extent — from its current near face to its previous far face,
// using `speed * dt` as the per-frame z delta — so a fast obstacle
// that would otherwise tunnel through the ship in a single frame
// still registers as overlap.
//
// Kind dispatch decides the response per pool entry:
//   * `OBSTACLE_KIND_WALL` → always a scrape: sets
//     `scrape_left` / `scrape_right` and pushes the ship out by
//     `x_pen` so it physically can't penetrate the wall.
//   * `OBSTACLE_KIND_CUBE` → head-on iff the obstacle's near
//     face was *still ahead of the ship's front face at the
//     start of the frame* — i.e. the obstacle just slammed into
//     the ship from ahead. Returns true; caller flips the app
//     state to GAME_OVER. Otherwise (the obstacle was already
//     overlapping or past last frame), the contact is a
//     trailing scrape — push out and ramp speed.
//   * pickup / ramp stubs `continue` so they don't kill the
//     ship on contact. Phase 5 / 6 / 9 will fill these in.
bool game_collide(game_state_t* g, world_state_t* w, float dt);

// Sun + shadow + speed dynamics. Returns `true` when the run
// should end because the ship has coasted to a stop in shadow.
//
// Three things happen here:
//
// 1. Sun integration. `sun_y` ticks forward by
//    `(base_rate - speed_influence * (ship_speed - cruise)) * dt`,
//    clamped to `[0, GAME_SUN_SINK_RANGE_PX]`. Slower ship → faster
//    sunset; faster ship → slower sunset or even rising.
//
// 2. Shadow detection. `in_shadow` becomes true when any active
//    cube obstacle ahead of the ship casts a shadow long enough
//    to reach the ship, or when the sun has fully set
//    (everything in shadow).
//
// 3. Speed dynamics. In-shadow takes priority over scrape: linear
//    decel from cruise to zero in `GAME_SHADOW_STALL_SECONDS`.
//    Otherwise the existing scrape/recovery dynamics run.
//
// Returns true iff `ship_speed_z` has reached zero — caller flips
// the app state to GAME_OVER (same end-of-run path as a head-on
// collision).
bool game_after_collide(game_state_t* g, world_state_t const* w, float dt);

// Render the ship as a 3D mesh, banked by `g->bank`.
void game_draw_ship(pax_buf_t* fb, game_state_t const* g);

// Draw a radial burst of red lines from each scraping wingtip.
// No persistence — each call paints a fresh random pattern, so
// the effect is purely a per-frame indication of "this wingtip is
// in contact right now". Only meaningful during PLAYING; callers
// in other states should skip it.
void game_draw_sparks(pax_buf_t* fb, game_state_t const* g);

// Crash explosion. game_crash_burst fills the spark pool with an
// outward shower originating at the ship's projected screen
// position — call it once, on the head-on crash frame, in place of
// the ship. game_crash_tick integrates the sparks (outward motion,
// gravity, ageing) and returns true while any spark is still alive.
// game_draw_crash_sparks paints the live sparks as short streaks
// that shrink and cool from hot yellow to red ember as they fade.
void game_crash_burst(game_state_t* g);
bool game_crash_tick(game_state_t* g, float dt);
void game_draw_crash_sparks(pax_buf_t* fb, game_state_t const* g);
