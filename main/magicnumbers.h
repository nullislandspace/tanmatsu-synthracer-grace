#pragma once

// Display / framebuffer geometry — single source of truth.
//
// The Tanmatsu LCD is physically mounted rotated 270° from the
// user's viewpoint. `bsp_display_get_default_rotation()` returns
// BSP_DISPLAY_ROTATION_270, which we map to `PAX_O_ROT_CW`.
//
// _RAW_  values describe the LCD's native (unrotated) layout —
//        this is how the framebuffer is actually stored in PSRAM.
// _LOG_  values describe what code sees through PAX's coordinate
//        system, i.e. after the orientation transform.
//
// The relationship under PAX_O_ROT_CW is:
//     DISPLAY_LOG_W  ==  DISPLAY_RAW_H
//     DISPLAY_LOG_H  ==  DISPLAY_RAW_W
//
// Centralising these here lets the custom direct-565 line/pixel
// helpers (`direct_565.h`) hardcode rotation + stride into the
// inner loop while keeping the numeric values configurable — port
// to a different display by updating these defines.
#define DISPLAY_RAW_W       480
#define DISPLAY_RAW_H       800

#define DISPLAY_LOG_W       800   // == DISPLAY_RAW_H
#define DISPLAY_LOG_H       480   // == DISPLAY_RAW_W

// Raw-buffer stride, in pixels (not bytes). Equals DISPLAY_RAW_W
// because the framebuffer is a tightly-packed 2D RGB565 array.
#define DISPLAY_RAW_STRIDE  DISPLAY_RAW_W


// =============================================================
// Gameplay tuning — Phase 5 sun / shadow / stall mechanic.
// =============================================================
//
// All values are in seconds-at-cruise-speed where applicable.
// The actual world distance budgets (stage length, rest length)
// derive from `seconds × cruise speed`, so changing cruise speed
// re-paces the layout without retuning the time values.
//
// The sun's position is what determines how much time you have
// left — there's no separate countdown timer. At cruise speed the
// sun sinks `GAME_SUN_SINK_RANGE_PX` pixels in
// `GAME_SUNSET_SECONDS_AT_CRUISE` seconds. Going slower than
// cruise (e.g. shadow-stalled) makes the sun sink faster; going
// faster than cruise (boost) makes it sink slower or even rise.
//
// When the sun has fully set behind the mountains the whole world
// is treated as in-shadow, and the ship decelerates from cruise
// speed to zero over `GAME_SHADOW_STALL_SECONDS`. The run ends the
// moment ship speed reaches zero — so while coasting to a halt
// there's a window to grab a boost pickup and recover.

// Ship cruise speed, world units / second. Single source of truth;
// `SHIP_BASE_SPEED_Z` in game.h derives from this so the
// game-state default and the world generator agree. Bumping this
// makes the ship feel faster and stretches each stage's world-z
// distance proportionally so the in-seconds pacing stays the same.
#define GAMEPLAY_CRUISE_SPEED            20.0f

// Sun vertical travel from baseline (high) to fully set behind
// the mountains, in logical pixels of the framebuffer. The PPA
// pipeline shifts the sun cache's destination Y by this much
// across a full sunset. Tuned by visual inspection with the Q/A
// debug nudge: at sun_y = 160 the last sun band is fully obscured
// by the lowest mountain valley. Bumping this higher just delays
// the moment the "fully shadowed" gameplay state triggers.
#define GAME_SUN_SINK_RANGE_PX           160.0f

// Sunset duration at cruise speed. Sun travels
// GAME_SUN_SINK_RANGE_PX over this many seconds while the ship
// holds cruise speed. Slower → sun sinks faster, faster → slower
// or rises (catch-up via boost).
#define GAME_SUNSET_SECONDS_AT_CRUISE    70.0f

// How strongly ship speed deviation from cruise affects the sun's
// vertical motion. The full rate formula in game_after_collide is:
//
//   base_rate     = SUN_SINK_RANGE_PX / SUNSET_SECONDS_AT_CRUISE
//   coef          = (base_rate / GAMEPLAY_CRUISE_SPEED) * GAME_SUN_SPEED_INFLUENCE
//   d(sun_y)/dt   = base_rate - coef * (ship_speed - GAMEPLAY_CRUISE_SPEED)
//
// The speed at which the sun stops sinking is
// `cruise * (1 + 1/INFLUENCE)`; the rate at zero speed is
// `base_rate * (1 + INFLUENCE)`; and the rise rate at `2× cruise`
// is `base_rate * (INFLUENCE - 1)` (positive = rising).
//
//   INFLUENCE = 1.0 → freeze at 2 × cruise; stalled = 2× base rate;
//                     2× cruise = 0 rise.
//   INFLUENCE = 3.0 → freeze at ~1.33 × cruise; stalled = 4× base
//                     rate; 2× cruise = 2× base *rising* rate
//                     (default — matches the "boost catches up
//                     quickly" feel).
//   INFLUENCE = 2.0 → freeze at 1.5 × cruise; stalled = 3× base;
//                     2× cruise = 1× base rising.
#define GAME_SUN_SPEED_INFLUENCE         3.0f

// While the ship is in shadow, forward speed decays linearly
// toward zero. This many seconds from cruise speed to a dead
// stop. The player can still pick up a boost during this window
// to recover and chase the sun back up — game ends only when
// speed actually reaches zero, not when it enters shadow.
#define GAME_SHADOW_STALL_SECONDS         8.0f

// Shadow length as a multiplier on obstacle height. Linearly
// interpolated between MIN (sun at the top of the screen, near
// the start of the run) and MAX (sun about to fully set, with
// only a few pixels peeking above the mountains). Past full
// sunset the whole world is in shadow regardless of geometry.
//
//   sun_norm = sun_y / GAME_SUN_SINK_RANGE_PX   ∈ [0, 1]
//   factor   = lerp(MIN, MAX, sun_norm)
//   shadow_len_z = obstacle.height * factor
//
// Shadows extend from the obstacle toward the camera (since the
// sun is ahead of the ship). A ship at z < obs.z_world is in the
// shadow if its z-distance to the obstacle is less than the
// shadow length AND it overlaps laterally with the obstacle's
// footprint.
#define GAME_SHADOW_LEN_FACTOR_MIN        1.0f
#define GAME_SHADOW_LEN_FACTOR_MAX        6.0f

// Solid darker purple painted on the floor where an obstacle's
// shadow falls (and across the whole floor after full sunset).
// ARGB8888 — the rasterizer packs to RGB565 internally. Tunable
// for visual contrast against the regular floor base
// `0xFF5D0B8B`. Default chosen as a darker, desaturated variant
// of the same purple so shadows read as "floor in shade" rather
// than "another colored object".
#define GAME_SHADOW_FLOOR_COLOR           0xFF2D0855u

// Multiplier applied to each RGB channel of the ship sprite when
// the ship is in shadow. 0.7 = ~30% darker. Drives only the
// visual tint — the gameplay-side shadow stall is unrelated.
#define GAME_SHIP_SHADOW_TINT             0.7f

// Per-stage obstacle play time, seconds at cruise. Implemented
// as a world-z distance budget so a slow run takes longer in
// real time but encounters the same set of obstacles per stage.
#define GAME_STAGE_SECONDS               60.0f

// Rest / inter-stage / bonus area duration, seconds at cruise.
// Empty of obstacles today; Phase 5 sprinkles boost pickups
// here, Phase 6 adds tri pickups.
#define GAME_REST_SECONDS                10.0f

// Derived world-z distance budgets. Defined here so any module
// that thinks in distance (world.c) and any module that thinks
// in seconds (game.c, HUD) agree by construction. Changing
// cruise speed or the seconds values automatically updates
// these.
#define GAME_STAGE_LENGTH_Z  (GAME_STAGE_SECONDS * GAMEPLAY_CRUISE_SPEED)
#define GAME_REST_LENGTH_Z   (GAME_REST_SECONDS  * GAMEPLAY_CRUISE_SPEED)
