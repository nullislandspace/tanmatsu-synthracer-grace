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
// The DISPLAY_* constants moved into the engine's overridable config
// (synthengine3D/include/se_config.h) so the renderer owns them; they
// are included here so game code that pulls in magicnumbers.h still sees
// the same values. Override by defining before this header is reached.
#include "se_config.h"


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

// Vertical motion (Phase 9.1). The ship gains an altitude above its
// rest height; a jump injects GAME_JUMP_SPEED of upward velocity and
// GAME_GRAVITY pulls it back down. Deliberately floaty so the arc is
// easy to read and time: a jump lasts ~1.9 s airborne and peaks
// ~1.6 world units up, with a gentle ~3.3 u/s peak vertical speed.
// Lower both for an even slower arc; raise GAME_JUMP_SPEED alone for
// a higher one. Tune to taste.
#define GAME_GRAVITY                     3.5f    // world units / s^2, downward
#define GAME_JUMP_SPEED                  3.3f    // world units / s, upward kick

// Jump-charge inventory cap (Phase 9.1f). Collecting a jump booster
// adds one charge toward this cap; game_jump spends one per jump.
#define GAME_JUMP_CHARGE_MAX             3

// Ramp reachable band (Phase 9.1g): how far above the ship's belly
// a ramp surface may sit and still count as support. Wide enough to
// cover the per-frame climb while riding (ship_speed × slope × dt),
// narrow enough that the ship is never yanked onto a high ramp it
// did not drive up.
#define GAME_RAMP_STEP_UP                1.0f

// Landing tolerance (world units): vertical slop for treating a
// ship-belly-vs-obstacle-top contact as "resting on / landing on"
// the surface rather than a lethal collision. Absorbs float error
// in the landing snap so a ship riding a platform is never seen as
// penetrating it.
#define GAME_LAND_EPS                    0.02f

// Camera Y-follow fraction: the camera rises this fraction of the
// ship's jump altitude. 1.0 locks the camera a constant height
// above the ship — it stays slightly above the ship at all times
// and the ship holds a stable screen position. Lower values let the
// ship visibly rise in frame instead. Tunable.
#define GAME_CAM_Y_FOLLOW                1.0f

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
//
// **DO NOT** change this colour to be within ~7 quantization steps
// (out of 31 per channel for R/B, 63 for G) of the floor base
// `0xFF5D0B8B` in *any* channel. Today the two pack to RGB565
// 0x284A vs 0x5851, comfortably distinct on R and B. Main.c's
// shadow-under-ship sampler does a single uint16_t compare against
// the packed value of this constant to set `game.in_shadow` — if
// the floor base ever collapses to the same 565 word, the sampler
// silently reads "always in shadow". Same caveat applies if anything
// new in synthwave_step_base paints with a colour that quantises
// to 0x284A.
#define GAME_SHADOW_FLOOR_COLOR           0xFF2D0855u

// Multiplier applied to each RGB channel of the ship sprite when
// the ship is in shadow. 0.7 = ~30% darker. Drives only the
// visual tint — the gameplay-side shadow stall is unrelated.
#define GAME_SHIP_SHADOW_TINT             0.7f


// =============================================================
// Speed booster pickup — the recovery loop for Phase 5.
// =============================================================
//
// Collecting a booster forces the ship through three phases:
//   1. RAMP   — linear ramp from current speed to the target
//               speed over `RAMP_UP_SECONDS`.
//   2. HOLD   — pegged at the target speed for `HOLD_SECONDS`.
//   3. COAST  — linear decel back to base speed at
//               `COAST_DECEL`. Yields to shadow stall if the
//               ship is in shadow during this phase.
//
// During RAMP and HOLD the boost overrides every other speed
// dynamic (shadow stall, scrape recovery). During COAST the
// shadow stall takes priority again so the boost doesn't insulate
// the player from a fresh shadow encounter.

// Target speed during the boost's hold phase, world units / s.
// At the current `cruise = 20`, `INFLUENCE = 3.0` tuning this
// makes the sun *rise* at 2× the base sink rate while the boost
// is active, so a single boost reverses several seconds of
// natural sunset. Re-derive when cruise/influence change:
//   ship = cruise × (1 + 3/INFLUENCE)   for "rising at 2× base"
#define GAME_BOOST_TARGET_SPEED           40.0f

// Linear ramp from pickup-speed to target speed, seconds.
#define GAME_BOOST_RAMP_UP_SECONDS         2.0f

// How long the ship is held at the target speed after the ramp.
#define GAME_BOOST_HOLD_SECONDS            1.0f

// Linear coast-back deceleration, world units per second². At
// the default 2.0, going from 40 → 20 takes 10 s — slower than
// scrape decel (5 u/s²) so a successful boost gives a long tail
// of above-cruise travel that keeps the sun off-balance.
#define GAME_BOOST_COAST_DECEL             2.0f

// Number of boosters spawned per stage, distributed roughly
// evenly along the stage's world-z budget (with per-segment
// jitter from the stage PRNG).
#define GAME_BOOSTERS_PER_STAGE            4

// First stage on which a shield pickup appears. One shield is
// scheduled per stage from this stage onward (Phase 9.2).
#define GAME_SHIELD_FIRST_STAGE            3

// First stage on which a checkpoint pickup appears. One checkpoint
// is scheduled per stage from this stage onward (Phase 9.3).
#define GAME_CHECKPOINT_FIRST_STAGE        5

// Shield inventory cap (HUD: violet hexagon) and effect tuning
// (Phase 9.2). The player holds at most one shield. On a head-on
// crash that charge opens a GAME_SHIELD_DURATION-second
// invulnerability window; crashes are survivable during it.
// GAME_SHIELD_HIT_COOLDOWN debounces the crash SFX / spark burst so
// a sustained smash doesn't retrigger them every frame.
#define GAME_SHIELD_CHARGE_MAX             1
#define GAME_SHIELD_DURATION               4.0f
#define GAME_SHIELD_HIT_COOLDOWN           0.35f

// Magnet attachment (Phase 9.4). When the magnet is equipped,
// world_magnet_pull pulls every active pickup toward the ship — sideways
// onto its lane AND forward along the track — while the pickup sits
// inside the capture volume (forward / lateral / vertical reach) and at
// roughly the ship's elevation. ALL values are first-cut, tuned on-device.
//   _RADIUS_Z    — how far ahead (world-z) the magnet reaches. Pickups
//                  farther than this, or already behind the ship, are
//                  ignored. Bigger = the magnet grabs earlier.
//   _RADIUS_X    — lateral capture band (world-x, each side). A pickup
//                  must be within this |Δx| of the ship to be pulled;
//                  outside it the player still has to steer over.
//   _RADIUS_Y    — vertical band (world-y). The pickup's base must be
//                  within this |Δy| of the ship's belly, so the magnet
//                  ignores pickups on a platform the ship passes under,
//                  and ground pickups the ship is flying / jumping over.
//   _PULL_RATE_X — lateral closing speed at the capture-band edge
//                  (world-x / s). Eases off linearly as the pickup nears
//                  the ship's x so it settles instead of jittering across
//                  the centreline; clamped so it never overshoots.
//   _PULL_RATE_Z — forward closing speed (world-z / s) added ON TOP of
//                  the normal world scroll, reeling the pickup toward the
//                  ship; clamped at the ship's z plane so it never drags
//                  a pickup behind the ship.
#define GAME_MAGNET_RADIUS_Z               18.0f
#define GAME_MAGNET_RADIUS_X               4.4f
#define GAME_MAGNET_RADIUS_Y               1.0f
#define GAME_MAGNET_PULL_RATE_X            8.0f
#define GAME_MAGNET_PULL_RATE_Z            8.0f

// Battery upgrade (Phase 9.5). The battery is a shadow buffer: while it
// holds charge, being in shadow (obstacle/terrain shadow OR a fully-set
// sun) does not stall the ship — instead the battery discharges. In the
// light it recharges. Charge is a 0..GAME_BATTERY_MAX scalar; each
// GAME_BATTERY_PER_INDICATOR slice lights one of the ship's four
// charge-indicator cells (the cell mid-discharge fades white→black over
// its slice). The run cap (meta.battery_max_charge) is set per level in
// Phase 11; for now it defaults to GAME_BATTERY_MAX so the battery is
// testable. Discharge and recharge share GAME_BATTERY_RATE.
#define GAME_BATTERY_MAX                   100.0f
#define GAME_BATTERY_PER_INDICATOR         25.0f
#define GAME_BATTERY_RATE                  25.0f   // charge units / s (both ways)

// Number of boosters in each inter-stage rest area.
#define GAME_BOOSTERS_PER_REST             1

// Booster geometry — collision AABB stays a 0.8×0.8×0.8 box (the
// HALF_W of 0.4 matches a pixel-cube footprint). Visual is a
// slowly-rotating regular icosahedron inscribed in that box (see
// `render_booster_icosahedron` in render.c). Phase 6 (2026-05-14)
// replaced the original pyramid visual; collision math unchanged.
#define GAME_BOOSTER_HALF_W                0.4f
#define GAME_BOOSTER_HEIGHT                (2.0f * GAME_BOOSTER_HALF_W)

// Booster colour palette. Bright neon green; faceted-gem lighting
// in the renderer tints each face by its normal direction so the
// icosahedron reads as a 3D crystal rather than a flat blob.
#define GAME_BOOSTER_FRONT_COLOR           0xFF60FF60u
#define GAME_BOOSTER_SIDE_COLOR            0xFF20A040u
#define GAME_BOOSTER_OUTLINE_COLOR         0xFFC0FFC0u

// Booster rotation period — full Y-axis rotation per second.
// Slow enough to read each face cleanly, fast enough to draw the
// eye. All boosters spin in lockstep using a global time source,
// so no per-instance phase storage is needed for the visual
// (only the per-instance position/state matters for collision).
#define GAME_BOOSTER_ROTATION_PERIOD_S     1.0f

// Tri pickup (Phase 6). Geometry mirrors the original booster
// pyramid: same footprint as a pixel-cube, apex at base-width
// above the floor. Different colour so it reads as obviously
// different from the icosahedron booster.
#define GAME_TRI_HALF_W                    0.4f
#define GAME_TRI_HEIGHT                    (2.0f * GAME_TRI_HALF_W)

// Tri colour palette — synthwave cyan-blue family. Front face
// is the brightest cyan, side faces are a darker blue, outline
// is near-white so the silhouette pops against the playfield.
#define GAME_TRI_FRONT_COLOR               0xFF60E0FFu
#define GAME_TRI_SIDE_COLOR                0xFF2060B0u
#define GAME_TRI_OUTLINE_COLOR             0xFFC0E8FFu

// Phase 6 scoring constants.
//
// Per-frame distance income: `score += dz × multiplier ×
// GAME_SCORE_DISTANCE_FACTOR` in `game_after_collide`. The
// factor scales the always-on baseline relative to the
// discrete pickup bumps below — with the factor at 0.1, a
// pickup at multiplier=1 feels like ~50 world-units of
// distance (= 50 × 0.1 = 5 score units, matching a Tri's
// bonus), so the player perceives Tris as meaningful events
// rather than rounding-error-on-top-of-the-distance-counter.
//
// Tri / booster pickup bonuses are scaled by the current
// multiplier at the moment of pickup. The multiplier itself
// increments on every 5th Tri.
#define GAME_SCORE_DISTANCE_FACTOR         0.1f
#define GAME_SCORE_TRI                     5
#define GAME_SCORE_BOOSTER                 10

// Crash penalty applied to `game.multiplier` on a head-on / stall
// run-end. Floors at 1 today; Phase 11 will raise the floor with
// player level (lv6 → 2, lv12 → 3, lv23 → 4, lv24 → max).
#define GAME_MULTIPLIER_CRASH_PENALTY      5
#define GAME_MULTIPLIER_FLOOR              1

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


// =============================================================
// Audio gain staging — applied by the mixer (audio_mixer.c).
// =============================================================
//
// Linear-amplitude master gains for the two contributing groups
// (music stream + one-shot SFX voices). The engine hum is itself
// a voice, so it goes through the SFX gain. The mixer applies
// these *after* each source has rendered its samples and *before*
// summing into the accumulator — so individual SFX `*_AMP`
// constants stay tuned to their own dynamics (relative loudness
// between a ding and a crash), and the master gains here set the
// overall balance against music + the int16 clip ceiling.
//
// Headroom budget: the mixer hard-clips at ±1.0 (int16 ±32767),
// and we want to stay clean with up to **5 concurrent SFX +
// music + engine hum** firing on the same frame. Random-phase
// summing scales peak by √N, so 5 random-phase voices at
// effective peak A sit around A·√5 ≈ 2.24·A; worst-case in-phase
// is 5·A. With AUDIO_SFX_GAIN = 0.35 and per-voice nominal amps
// in the 0.4–0.6 range, effective per-voice peaks are 0.14–0.21,
// 5-voice RMS ≈ 0.31–0.47, leaving ~0.5 for the music + hum bed
// (music at ~0.30, hum well under 0.05).
//
// Perceived loudness is logarithmic, not linear: each 0.5×
// amplitude step is -6 dB, "twice as loud" is closer to ×3.16
// (+10 dB). So bigger numeric steps here translate to subtler
// perceived changes than instinct suggests. Tune by ear, then
// verify headroom math on paper rather than the other way around.
// AUDIO_MUSIC_GAIN / AUDIO_SFX_GAIN now live in the engine config
// (se_config.h, included above) as overridable defaults; the headroom
// reasoning above is retained here for context. Override them per game
// by defining before se_config.h is reached.


// =============================================================
// Synthwave backdrop / PPA compositor layout.
// =============================================================
//
// Geometry + colours for the PPA-driven synthwave backdrop (sky FILL,
// sun SRM, mountain BLEND) implemented in backdrop.{c,h}. These key off
// the display geometry above (DISPLAY_LOG_W / the horizon line), so they
// live here with the rest of the layout tunables rather than buried in
// the backdrop translation unit.

// (The PSRAM cache-line size for the PPA layer caches now lives in the
// engine as SE_PPA_CACHE_LINE -- se_ppa allocates + flushes the caches.)

// Sky region in logical coordinates. `synthwave_draw_top_grid` paints
// the magenta line at y = HORIZON_LOGICAL_Y; `synthwave_step` paints
// the floor starting at y = HORIZON_LOGICAL_Y + 1. The PPA pipeline
// touches logical rows [0, HORIZON_LOGICAL_Y], i.e. SKY_ROWS rows.
#define HORIZON_LOGICAL_Y 256
#define SKY_ROWS          (HORIZON_LOGICAL_Y + 1)

// Sun cache: tight bounding box of the sun bands at their canonical
// baseline. Bands span fb logical y = -4 (off-screen above) to ~174;
// we render with y_bias = +4 so the topmost band lands at cache y=0
// and the cache is exactly tall enough to hold the whole sun.
#define SUN_CACHE_LOG_W   DISPLAY_LOG_W
#define SUN_CACHE_LOG_H   180
#define SUN_RENDER_Y_BIAS 4.0f

// Mountain cache: tight bounding box of the visible mountain band.
// The band spans fb logical y = 94 (mountain peaks) down to 256
// (horizon). The cache is rendered with y_bias = -94 so the top of
// the visible mountain region lands at cache y=0, and the horizon
// line at fb y=256 lands at cache y=162.
#define MOUNTAIN_CACHE_LOG_W   DISPLAY_LOG_W
#define MOUNTAIN_CACHE_LOG_H   163
#define MOUNTAIN_RENDER_Y_BIAS (-94.0f)
#define MOUNTAIN_DEST_LOG_Y    94

// Colour-key for the mountain cache background. Pure green never
// appears in the synthwave palette (purples / pinks / cyans /
// oranges / magentas), so a tight key around it can't false-match
// any artwork pixel. The cache stores RGB565 (5-6-5); PPA expands
// to RGB888 internally before comparing against the thresholds.
// Whether the expansion is "shift" (g=0x3F -> 0xFC) or "replicate"
// (g=0x3F -> 0xFF) varies by hardware revision, so the threshold
// range covers both: low (0,0xFC,0) -- high (0,0xFF,0).
#define MOUNTAIN_KEY_PAX_COL 0xFF00FF00u

// Sky colour for PPA FILL. Same purple PAX paints with
// `pax_background(0xFF552075)`.
#define SKY_PAX_COL 0xFF552075u
