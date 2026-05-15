#include "game.h"

#include <math.h>
#include <string.h>

#include "pax_gfx.h"
#include "render.h"
#include "shapes/pax_tris.h"

// --- Lateral motion -----------------------------------------------------------

// SHIP_X_MIN_WORLD / SHIP_X_MAX_WORLD live in game.h because
// game_collide consults them to tell boundary obstacles (side
// walls) from playfield obstacles.

// Visual maximum bank in radians (~31°). Multiplied by `bank` to
// get the rendered roll angle.
#define MAX_BANK_RAD          0.55f

// Bank ramp rates. ACTIVE applies whenever any steer direction is
// held (so reversing the stick snaps the ship over fast); PASSIVE
// is the slower self-righting rate when the stick is released.
#define BANK_ACTIVE_RATE      3.5f
#define BANK_PASSIVE_RATE     1.0f

// Lateral world-units travelled per second at full bank.
#define SHIP_TURN_RATE        3.5f

// Ship lateral half-width at the current bank. The mesh wing tips
// sit at local x = ±SHIP_COLLISION_HALF_W; banking rolls them out
// of the horizontal plane, so the projected half-width shrinks by
// cos(bank angle). Used to clamp the ship's *edge* — not its
// centre — against the border walls, so a banked ship can tuck its
// dipped wing nearer the wall while an upright one keeps clear.
static inline float ship_lateral_half_w(float bank) {
    return SHIP_COLLISION_HALF_W * cosf(bank * MAX_BANK_RAD);
}

// --- Speed dynamics -----------------------------------------------------------

// Scrape behaviour: while either scrape flag is set, ship_speed_z
// ramps toward (base * SCRAPE_SPEED_FRAC) at SCRAPE_DECEL u/s².
// When neither flag is set it ramps back up to base at the slower
// SPEED_RECOVERY rate. Both transitions are continuous — never a
// step change — so grazing a wall for a fraction of a second only
// sheds a fraction of the full slowdown, and recovery is visible
// over time. Recovery is deliberately slower than decel so scrapes
// feel punishing.
#define SCRAPE_SPEED_FRAC     0.55f
#define SCRAPE_DECEL          5.0f
#define SPEED_RECOVERY        2.5f

// --- Ship mesh visuals --------------------------------------------------------

#define SHIP_ROOF_LEFT_COLOR   0xFFFFFF6Bu
#define SHIP_ROOF_RIGHT_COLOR  0xFFD8AA38u
#define SHIP_BELLY_COLOR       0xFFF71FF1u
#define SHIP_RIDGE_COLOR       0xFF31FBFBu

typedef struct {
    float x, y, z;
} ship_vert_t;

static ship_vert_t const ship_verts[] = {
    [0] = {  0.00f,  0.30f,   0.32f},  // nose (elevated apex)
    [1] = { -0.28f,  0.00f,  -0.10f},  // left  wing tip
    [2] = {  0.28f,  0.00f,  -0.10f},  // right wing tip
    [3] = {  0.00f,  0.00f,  -0.36f},  // tail
};

typedef enum {
    SHIP_FACE_BELLY = 0,
    SHIP_FACE_ROOF_LEFT,
    SHIP_FACE_ROOF_RIGHT,
} ship_face_t;

typedef struct {
    uint8_t     a, b, c;
    ship_face_t face;
} ship_tri_t;

static ship_tri_t const ship_tris[] = {
    {0, 2, 1, SHIP_FACE_BELLY},
    {1, 2, 3, SHIP_FACE_BELLY},
    {0, 1, 3, SHIP_FACE_ROOF_LEFT},
    {0, 3, 2, SHIP_FACE_ROOF_RIGHT},
};

static uint8_t const ship_outline_edges[][2] = {
    {0, 1}, {1, 3}, {3, 2}, {2, 0}, {0, 3},
};

#define SHIP_VERT_COUNT    (sizeof(ship_verts) / sizeof(ship_verts[0]))
#define SHIP_TRI_COUNT     (sizeof(ship_tris)  / sizeof(ship_tris[0]))
#define SHIP_OUTLINE_COUNT (sizeof(ship_outline_edges) / sizeof(ship_outline_edges[0]))

// --- Sparks -------------------------------------------------------------------

// Wingtip world-z offset, so the burst origin sits at the leading
// edge of the wing rather than the geometric centre of the ship.
#define SPARK_EMIT_DZ      (-0.10f)
// Lateral half-extent of the mesh wing tip (matches ship_verts[1].x
// magnitude and SHIP_COLLISION_HALF_W). Used as the local x of the
// emission point so the projected origin matches the rendered wing
// tip after the bank rotation.
#define SPARK_WING_HALF_W  0.28f
// Lines drawn per scraping wingtip per frame.
#define SPARK_LINES        5
// Spark line length range in screen pixels. Bumped from the
// original 5..10 because at game distance that read too small —
// individual streaks were lost against the ship and floor.
#define SPARK_LEN_MIN      10.0f
#define SPARK_LEN_MAX      22.0f
// Bright red — single colour, no fade or palette ramp. Stark
// against both the ship's yellow roof and the floor stripes.
#define SPARK_COLOR        0xFFFF3030u

// PRNG state for the visual-only spark direction/length pick. Kept
// at module scope rather than in game_state_t because the sparks
// are pure ephemera — nothing in the game state needs to know about
// them between frames.
static uint32_t s_spark_rng = 0xC0FFEEu;

static float spark_rand(void) {
    uint32_t x = s_spark_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_spark_rng = x ? x : 1u;
    return (float)x / 4294967296.0f;
}

static void draw_wingtip_burst(pax_buf_t* fb, game_state_t const* g, int side) {
    // The wing tip's world position has to match the rendered
    // wing tip *after* the ship's bank rotation about the +z
    // axis — same 2D rotation game_draw_ship applies to the
    // mesh vertices. Without it, sparks emit from the level-
    // flight position and visibly drift away from the wing when
    // the ship is banking.
    float const bank_angle = g->bank * MAX_BANK_RAD;
    float const c          = cosf(bank_angle);
    float const s          = sinf(bank_angle);
    float const local_x    = (float)side * SPARK_WING_HALF_W;
    // Wing tip's local y is 0; only the rotated x and -x*sin
    // matter. Matches the projection math in game_draw_ship.
    float const wx = local_x * c + g->ship_x_world;
    float const wy = -local_x * s + SHIP_BASE_Y;
    float const wz = SHIP_Z_PLANE + SPARK_EMIT_DZ;

    float sx, sy;
    render_project(wx, wy, wz, &sx, &sy);

    for (int i = 0; i < SPARK_LINES; i++) {
        float const angle = spark_rand() * 6.28318531f;
        float const len   = SPARK_LEN_MIN + spark_rand() * (SPARK_LEN_MAX - SPARK_LEN_MIN);
        float const ex    = sx + cosf(angle) * len;
        float const ey    = sy + sinf(angle) * len;
        pax_simple_line(fb, SPARK_COLOR, sx, sy, ex, ey);
    }
}

// --- Public API ---------------------------------------------------------------

void game_init(game_state_t* g) {
    memset(g, 0, sizeof(*g));
    g->ship_speed_z      = SHIP_BASE_SPEED_Z;
    g->ship_base_speed_z = SHIP_BASE_SPEED_Z;
    g->multiplier        = 1;
    g->multiplier_max    = 1;
}

void game_step(game_state_t* g, world_state_t const* w, float dt, float steer) {
    if (dt <= 0.0f) return;

    // --- Bank dynamics ----------------------------------------------------
    // `steer` is a proportional deflection in [-1, +1]; the bank
    // chases it. "Active" (faster) ramp whenever the player is
    // asking for any deflection at all, "passive" recentre otherwise.
    float const bank_target   = steer;
    float const bank_rate     = (steer != 0.0f) ? BANK_ACTIVE_RATE : BANK_PASSIVE_RATE;
    float const bank_max_step = bank_rate * dt;
    float       bank_delta    = bank_target - g->bank;
    if (bank_delta >  bank_max_step) bank_delta =  bank_max_step;
    if (bank_delta < -bank_max_step) bank_delta = -bank_max_step;
    g->bank += bank_delta;

    // --- Lateral motion ---------------------------------------------------
    // Turn rate scales linearly with forward speed: at cruise the
    // lateral rate equals the tunable SHIP_TURN_RATE; at boost
    // (2× cruise) it doubles, so the ship maintains the same
    // lateral-to-longitudinal ratio at any speed. Without this,
    // a flat lateral rate over 2× the forward distance reads as
    // pronounced understeer. At stall the rate drops toward zero,
    // which is physically consistent — you can't bank-turn a ship
    // that isn't moving forward — and adds tension to shadow stalls.
    float const turn_rate = SHIP_TURN_RATE * (g->ship_speed_z / GAMEPLAY_CRUISE_SPEED);
    g->ship_x_world += g->bank * turn_rate * dt;
    // The lateral clamp IS the border wall (Phase 9.1e — the side
    // walls are no longer collision obstacles). It is infinite-
    // height by construction, so the ship can never leave or jump
    // over the track edge; game_collide turns a ship pinned here
    // while still banking outward into a wall-scrape. The clamp
    // bounds the ship's *edge*, not its centre: the centre stops a
    // bank-adjusted half-width short of the wall (a full half-width
    // when level), so the hull never overlaps the wall.
    float const half_w  = ship_lateral_half_w(g->bank);
    float const x_max_c = SHIP_X_MAX_WORLD - half_w;
    float const x_min_c = SHIP_X_MIN_WORLD + half_w;
    if (g->ship_x_world > x_max_c) {
        g->ship_x_world = x_max_c;
    } else if (g->ship_x_world < x_min_c) {
        g->ship_x_world = x_min_c;
    }
    g->cam_x = g->ship_x_world;

    // --- Vertical motion + landing ---------------------------------------
    // The ship's support surface is the highest landable obstacle
    // top its x-z footprint sits over, or the floor (support_y = 0,
    // in ship_y units). It is recomputed every frame, so riding off
    // a platform — the platform scrolls past, or the player steers
    // off its side — simply drops the support and the ship falls,
    // with no per-obstacle bookkeeping.
    float support_y = 0.0f;
    {
        float const ship_xL = g->ship_x_world - SHIP_COLLISION_HALF_W;
        float const ship_xR = g->ship_x_world + SHIP_COLLISION_HALF_W;
        float const ship_zN = SHIP_COLLISION_Z_C - SHIP_COLLISION_HALF_D;
        float const ship_zF = SHIP_COLLISION_Z_C + SHIP_COLLISION_HALF_D;
        for (int i = 0; i < WORLD_OBSTACLE_POOL_SIZE; i++) {
            obstacle_t const* o = &w->obstacles[i];
            if (!o->active) continue;
            // Only solid bodies are landable; pickups are not, and
            // ramps get their own sloped-surface handling in 9.1g.
            // Border walls aren't KIND_CUBE and sit outside the
            // track, so the x test below would exclude them anyway.
            if (o->kind != OBSTACLE_KIND_CUBE) continue;
            if (o->x_world - o->half_w >= ship_xR) continue;
            if (o->x_world + o->half_w <= ship_xL) continue;
            if (o->z_world - o->half_d >= ship_zF) continue;
            if (o->z_world + o->half_d <= ship_zN) continue;
            // Top face in ship_y units. Only a support if it is at
            // or below the ship's belly — a top above the belly is
            // an obstacle the ship will *collide* with, not stand on.
            float const top = (o->y_base + o->height) - SHIP_BASE_Y;
            if (top <= g->ship_y + GAME_LAND_EPS && top > support_y) {
                support_y = top;
            }
        }
    }

    // Rest on the support, or fall toward it. A jump (ship_vy > 0)
    // always leaves the surface; gravity pulls a descending ship
    // back down, and the descent snaps onto the support the frame
    // it crosses it — that snap is the landing.
    if (g->ship_y <= support_y && g->ship_vy <= 0.0f) {
        g->ship_y        = support_y;
        g->ship_vy       = 0.0f;
        g->ship_grounded = true;
    } else {
        g->ship_grounded = false;
        g->ship_vy      -= GAME_GRAVITY * dt;
        g->ship_y       += g->ship_vy * dt;
        if (g->ship_y <= support_y && g->ship_vy <= 0.0f) {
            g->ship_y        = support_y;
            g->ship_vy       = 0.0f;
            g->ship_grounded = true;
        }
    }
}

void game_jump(game_state_t* g) {
    // Needs a support surface AND a jump charge in hand (Phase
    // 9.1f) — collect jump boosters to refill. No double-jump:
    // `ship_grounded` is cleared on lift-off.
    if (g->ship_grounded && g->jump_charges > 0) {
        g->ship_vy       = GAME_JUMP_SPEED;
        g->ship_grounded = false;
        g->jump_charges -= 1;
    }
}

bool game_collide(game_state_t* g, world_state_t* w, float dt) {
    g->scrape_left  = false;
    g->scrape_right = false;
    g->just_picked_up_booster = false;
    g->just_picked_up_tri     = false;
    bool head_on    = false;

    float const ship_zN = SHIP_COLLISION_Z_C - SHIP_COLLISION_HALF_D;
    float const ship_zF = SHIP_COLLISION_Z_C + SHIP_COLLISION_HALF_D;
    // Ship vertical extent (Phase 9.1b). The collision box rises
    // with the jump: belly at SHIP_BASE_Y + ship_y, SHIP_COLLISION_HEIGHT
    // tall. ship_y is constant across this pass (the loop only nudges
    // ship_x_world), so the Y range is computed once here.
    float const ship_yB = SHIP_BASE_Y + g->ship_y;
    float const ship_yT = ship_yB + SHIP_COLLISION_HEIGHT;

    // Edge-aware border-wall bounds (Phase 9.1e). The ship's centre
    // comes no closer to a wall than its bank-adjusted half-width
    // (a full half-width when level); bank is constant across this
    // pass, so compute the bounds once.
    float const ship_half_w  = ship_lateral_half_w(g->bank);
    float const ship_x_max_c = SHIP_X_MAX_WORLD - ship_half_w;
    float const ship_x_min_c = SHIP_X_MIN_WORLD + ship_half_w;

    // Per-frame z motion. Obstacles will move by this much in
    // world_advance later this frame; they moved by approximately
    // the same amount last frame too. We use it for both swept-z
    // overlap detection (so fast obstacles can't tunnel between
    // frames) and to recover the obstacle's previous position for
    // the head-on / scrape classifier.
    float const dz_frame = (dt > 0.0f) ? (g->ship_speed_z * dt) : 0.0f;

    // Shadow ray (Phase 9.1d). `in_shadow` is the gameplay shadow
    // flag — true when an obstacle sits between the ship and the
    // sun. The sun direction is back-tracked from the renderer's
    // shadow-length math: a caster of height h drops a shadow
    // h*factor long toward the camera, so the direction *to* the sun
    // is (0, 1, factor) — zero lateral, +y, +z. Each KIND_CUBE
    // obstacle is ray-tested in the loop below (folded in, so no
    // second pool traversal); the first hit sets `shadowed`. The
    // post-sunset override lives in game_after_collide.
    bool        shadowed = false;
    float const factor   = GAME_SHADOW_LEN_FACTOR_MIN
                         + (GAME_SHADOW_LEN_FACTOR_MAX - GAME_SHADOW_LEN_FACTOR_MIN)
                           * (g->sun_y / GAME_SUN_SINK_RANGE_PX);
    // Ray origin: the ship's collision-box centre. Captured once —
    // the loop's lateral push-outs are sub-frame nudges irrelevant
    // to the shadow test.
    float const ray_x0 = g->ship_x_world;
    float const ray_y0 = ship_yB + SHIP_COLLISION_HEIGHT * 0.5f;
    float const ray_z0 = SHIP_COLLISION_Z_C;

    for (int i = 0; i < WORLD_OBSTACLE_POOL_SIZE; i++) {
        obstacle_t* o = &w->obstacles[i];
        if (!o->active) continue;
        // Border walls (Phase 9.1e) are not collision obstacles —
        // the lateral clamp in game_step is the wall, and the
        // wall-scrape is derived from it at the tail of this
        // function. Skip KIND_WALL entirely: collision, the shadow
        // ray and the support scan all ignore it.
        if (o->kind == OBSTACLE_KIND_WALL) continue;

        // Ship x bounds are recomputed each iteration because a
        // previous push-out may have shifted ship_x_world.
        float const ship_xL = g->ship_x_world - SHIP_COLLISION_HALF_W;
        float const ship_xR = g->ship_x_world + SHIP_COLLISION_HALF_W;

        float const obs_xL      = o->x_world - o->half_w;
        float const obs_xR      = o->x_world + o->half_w;
        // Current z range and the obstacle's last-frame z range
        // (positions are reduced by `dz_frame` each world_advance,
        // so previous position = current + dz_frame). The swept
        // range covers everywhere the obstacle was during the
        // frame: from its current near face down to its previous
        // far face. Catches single-frame tunneling at any speed.
        float const obs_zN_curr = o->z_world - o->half_d;
        float const obs_zF_curr = o->z_world + o->half_d;
        float const obs_zN_prev = obs_zN_curr + dz_frame;
        float const obs_zF_prev = obs_zF_curr + dz_frame;
        float const swept_zN    = obs_zN_curr;  // smallest z reached this frame (current near)
        float const swept_zF    = obs_zF_prev;  // largest  z reached this frame (previous far)

        float const obs_yB = o->y_base;
        float const obs_yT = o->y_base + o->height;

        // Shadow ray vs this obstacle (Phase 9.1d). Only solid cubes
        // cast a gameplay shadow — the same set the renderer shadows.
        // The ray has zero lateral component, so first gate on the
        // ship's x being inside the obstacle's x-span, then a y/z
        // slab test: dy/dt = 1, dz/dt = factor (> 0). A hit reaching
        // t >= 0 means the obstacle is between the ship and the sun.
        if (!shadowed && o->kind == OBSTACLE_KIND_CUBE
            && ray_x0 >= obs_xL && ray_x0 <= obs_xR) {
            float const t_lo = fmaxf(obs_yB - ray_y0,
                                     (obs_zN_curr - ray_z0) / factor);
            float const t_hi = fminf(obs_yT - ray_y0,
                                     (obs_zF_curr - ray_z0) / factor);
            if (t_hi >= 0.0f && t_lo <= t_hi) {
                shadowed = true;
            }
        }

        float const x_pen = fminf(ship_xR, obs_xR) - fmaxf(ship_xL, obs_xL);
        float const y_pen = fminf(ship_yT, obs_yT) - fmaxf(ship_yB, obs_yB);
        float const z_pen = fminf(ship_zF, swept_zF) - fmaxf(ship_zN, swept_zN);
        // Y-aware (Phase 9.1b): no vertical overlap ⇒ the ship is
        // clearing the obstacle — flying over it, or under a raised
        // one — so there is no collision at all.
        if (x_pen <= 0.0f || y_pen <= 0.0f || z_pen <= 0.0f) continue;

        // Top contact (Phase 9.1c): the ship's belly is at or above
        // this obstacle's top face — it is resting on / landing on
        // the top, not ramming the body. game_step does the vertical
        // resolution; here it is simply a ridable surface, neither
        // lethal nor a scrape.
        if (ship_yB >= obs_yT - GAME_LAND_EPS) continue;

        // Head-on iff the obstacle's near face was still ahead of
        // the ship's front face at the start of this frame — it
        // entered the ship's z range *from ahead* during the
        // frame. If the obstacle was already overlapping (or past)
        // last frame, this is a trailing scrape: the obstacle is
        // continuing past us, just push the ship out laterally.
        // The earlier "obstacle.z_world > SHIP_COLLISION_Z_C" rule
        // misclassified fast head-on hits where dz outran the
        // overlap window in one frame and the cube landed past
        // ship centre with the player still aimed dead at it.
        bool const came_from_ahead = obs_zN_prev >= ship_zF;

        // Per-object collision callback takes precedence; if the
        // object hasn't set one, fall back to kind-dispatched
        // default behaviour. Either way the result is a tristate the
        // outer loop turns into the right physics response.
        obstacle_hit_result_t hit;
        if (o->collide) {
            hit = o->collide(o, g, came_from_ahead);
        } else {
            switch (o->kind) {
                // OBSTACLE_KIND_WALL is filtered out at the top of
                // the loop (9.1e) and never reaches this dispatch.
                case OBSTACLE_KIND_CUBE:
                    // Hit from ahead → head-on (fatal). Already-passing
                    // → trailing scrape (push out and slow).
                    hit = came_from_ahead ? OBSTACLE_HIT_HEAD_ON : OBSTACLE_HIT_SCRAPE;
                    break;
                case OBSTACLE_KIND_PICKUP_BOOST:
                    // Collect the booster: deactivate the obstacle, kick
                    // the boost state machine into RAMPING from the
                    // current speed. Picking up another booster
                    // mid-boost just restarts at RAMPING from the new
                    // current speed (which might already be near the
                    // target). Phase 6: also add the booster's base
                    // points to the score, scaled by the current
                    // multiplier.
                    obstacle_despawn(o);
                    g->boost_phase            = BOOST_RAMPING;
                    g->boost_phase_time       = GAME_BOOST_RAMP_UP_SECONDS;
                    g->boost_ramp_start_speed = g->ship_speed_z;
                    g->pickups_speed_boost   += 1;
                    g->score                 += (double)GAME_SCORE_BOOSTER * (double)g->multiplier;
                    g->just_picked_up_booster = true;
                    hit                       = OBSTACLE_HIT_IGNORE;
                    break;
                case OBSTACLE_KIND_PICKUP_TRI:
                    // Phase 6 Tri pickup. Increment the monotonic
                    // per-run counter, award TRI base points × the
                    // *pre-bump* multiplier (the multiplier hasn't
                    // ticked yet for this pickup), then tick the
                    // multiplier on every 5th Tri. Slot index for
                    // the plink SFX is `(pickups_tri - 1) % 5` —
                    // 0..4 across the five pickups in a cycle.
                    obstacle_despawn(o);
                    g->score          += (double)GAME_SCORE_TRI * (double)g->multiplier;
                    g->pickups_tri    += 1;
                    g->tri_pickup_slot = (g->pickups_tri - 1) % 5;
                    g->just_picked_up_tri = true;
                    if (g->pickups_tri % 5 == 0) {
                        g->multiplier += 1;
                        if (g->multiplier > g->multiplier_max) {
                            g->multiplier_max = g->multiplier;
                        }
                    }
                    hit = OBSTACLE_HIT_IGNORE;
                    break;
                case OBSTACLE_KIND_PICKUP_JUMP:
                    // Phase 9.1f jump booster. Grant one jump charge
                    // (capped), count the pickup for run stats, and
                    // reuse the booster-pickup audio edge flag so it
                    // plays the same ding as a speed booster.
                    obstacle_despawn(o);
                    g->pickups_jump += 1;
                    if (g->jump_charges < GAME_JUMP_CHARGE_MAX) {
                        g->jump_charges += 1;
                    }
                    g->just_picked_up_booster = true;
                    hit = OBSTACLE_HIT_IGNORE;
                    break;
                case OBSTACLE_KIND_PICKUP_SHIELD:
                case OBSTACLE_KIND_RAMP:
                    // Not implemented yet — ignore so collision
                    // doesn't kill the ship on contact with a future
                    // pickup. Phase 9 / future will fill these in.
                    hit = OBSTACLE_HIT_IGNORE;
                    break;
                default:
                    hit = OBSTACLE_HIT_IGNORE;
                    break;
            }
        }
        if (hit == OBSTACLE_HIT_IGNORE) continue;

        if (hit == OBSTACLE_HIT_SCRAPE) {
            // Push the ship out of the obstacle laterally so it
            // physically can't penetrate. Side is decided by
            // which side of the ship the obstacle's centre lies
            // on. This also handles the wall case naturally —
            // any further bank-driven motion next frame will be
            // re-pushed back out, so the ship stays pressed
            // against the wall instead of going through it.
            if (o->x_world > g->ship_x_world) {
                g->scrape_right = true;
                g->ship_x_world -= x_pen;
            } else {
                g->scrape_left = true;
                g->ship_x_world += x_pen;
            }
            // Re-clamp inside the edge-aware lateral bounds, in
            // case the push-out shoved us past one of them.
            if (g->ship_x_world > ship_x_max_c) g->ship_x_world = ship_x_max_c;
            if (g->ship_x_world < ship_x_min_c) g->ship_x_world = ship_x_min_c;
        } else /* OBSTACLE_HIT_HEAD_ON */ {
            head_on = true;
        }
    }

    // Phase 6: crash penalty on multiplier. Applied once per
    // crash frame (head_on, set above) — only meaningful in
    // future scenarios where the player survives the hit
    // (e.g. Shield pickup absorbs one fatal contact, planned
    // for Phase 9+). Today the run ends on head_on so the
    // penalised multiplier never gets used, but we maintain
    // the rule so the gameplay invariant is correct from
    // the start. `multiplier_max` deliberately doesn't track
    // this drop — only increases bump the all-time peak.
    if (head_on) {
        g->multiplier -= GAME_MULTIPLIER_CRASH_PENALTY;
        if (g->multiplier < GAME_MULTIPLIER_FLOOR) {
            g->multiplier = GAME_MULTIPLIER_FLOOR;
        }
    }

    // Publish the shadow-ray result (Phase 9.1d). game_after_collide
    // forces this true once the sun has fully set.
    g->in_shadow = shadowed;

    // Border-wall scrape (Phase 9.1e). The side walls are not
    // collision obstacles — the lateral clamp in game_step is the
    // boundary. A ship pinned at a boundary while still banking
    // into it is grinding the wall: raise the scrape flag (same SFX
    // + wingtip sparks as an obstacle scrape). Banking away — or
    // coasting — leaves it clear, so the scrape ends the moment the
    // player stops pushing outward. Works at any altitude (X-only).
    if (g->ship_x_world >= ship_x_max_c && g->bank > 0.0f) {
        g->scrape_right = true;
    } else if (g->ship_x_world <= ship_x_min_c && g->bank < 0.0f) {
        g->scrape_left = true;
    }

    // Camera follows the resolved position.
    g->cam_x = g->ship_x_world;
    return head_on;
}

bool game_after_collide(game_state_t* g, world_state_t const* w, float dt) {
    if (dt <= 0.0f) return false;

    // --- Sun position update ---------------------------------------------
    // At cruise speed the sun travels GAME_SUN_SINK_RANGE_PX over
    // GAME_SUNSET_SECONDS_AT_CRUISE seconds. The per-frame sink
    // rate is linear in the speed differential from cruise. The
    // sensitivity to speed is controlled by GAME_SUN_SPEED_INFLUENCE
    // (default 1.0 → symmetric: stalled doubles the rate, 2× cruise
    // freezes the sun).
    {
        float const base_rate       = GAME_SUN_SINK_RANGE_PX / GAME_SUNSET_SECONDS_AT_CRUISE;
        float const speed_influence = (base_rate / GAMEPLAY_CRUISE_SPEED) * GAME_SUN_SPEED_INFLUENCE;
        float const rate            = base_rate - speed_influence * (g->ship_speed_z - GAMEPLAY_CRUISE_SPEED);
        g->sun_y += rate * dt;
        if (g->sun_y < 0.0f)                       g->sun_y = 0.0f;
        if (g->sun_y > GAME_SUN_SINK_RANGE_PX)     g->sun_y = GAME_SUN_SINK_RANGE_PX;
    }

    // --- Shadow detection ------------------------------------------------
    // The geometric shadow flag is computed by game_collide's shadow
    // ray (a ray from the ship toward the sun — first obstacle hit).
    // All that is left here is the post-sunset case: once the sun is
    // fully down the whole world is in shadow regardless of geometry,
    // so force the flag true.
    if (g->sun_y >= GAME_SUN_SINK_RANGE_PX) {
        g->in_shadow = true;
    }
    // (void)w retained because future logic may want the obstacle
    // pool again; the loop is gone today.
    (void)w;

    // --- Speed dynamics --------------------------------------------------
    // Priority order (top wins):
    //   1. Active boost RAMP/HOLD — overrides everything, ship is
    //      forced to the boost trajectory.
    //   2. Shadow stall — linear decel toward zero.
    //   3. Boost COAST  — slow linear decel back to base.
    //   4. Scrape / normal recovery — ramps toward base or scrape
    //      floor.
    if (g->boost_phase == BOOST_RAMPING) {
        // Linear lerp from the pickup speed up to the boost target
        // over GAME_BOOST_RAMP_UP_SECONDS. Boost overrides shadow,
        // scrape, everything — the whole point is to escape stalls.
        float const elapsed = GAME_BOOST_RAMP_UP_SECONDS - g->boost_phase_time;
        float const t       = elapsed / GAME_BOOST_RAMP_UP_SECONDS;
        g->ship_speed_z     = g->boost_ramp_start_speed
                            + (GAME_BOOST_TARGET_SPEED - g->boost_ramp_start_speed) * t;
        g->boost_phase_time -= dt;
        if (g->boost_phase_time <= 0.0f) {
            g->ship_speed_z     = GAME_BOOST_TARGET_SPEED;
            g->boost_phase      = BOOST_HOLDING;
            g->boost_phase_time = GAME_BOOST_HOLD_SECONDS;
        }
    } else if (g->boost_phase == BOOST_HOLDING) {
        // Peg the speed at target for the hold duration.
        g->ship_speed_z      = GAME_BOOST_TARGET_SPEED;
        g->boost_phase_time -= dt;
        if (g->boost_phase_time <= 0.0f) {
            g->boost_phase = BOOST_COASTING;
        }
    } else if (g->in_shadow) {
        // Shadow stall — wins over the boost COAST (a player who
        // re-enters shadow during the coast still pays the stall
        // cost). Linear decel toward zero.
        float const decel = GAMEPLAY_CRUISE_SPEED / GAME_SHADOW_STALL_SECONDS;
        g->ship_speed_z -= decel * dt;
        if (g->ship_speed_z < 0.0f) g->ship_speed_z = 0.0f;
        // If shadow stall drops us below base while coasting, drop
        // back to IDLE so the scrape-recovery path can resume on
        // shadow exit.
        if (g->boost_phase == BOOST_COASTING && g->ship_speed_z <= g->ship_base_speed_z) {
            g->boost_phase = BOOST_IDLE;
        }
    } else if (g->boost_phase == BOOST_COASTING) {
        // Slow linear coast back to base speed. Slower than the
        // scrape decel so a successful boost gives a long tail of
        // above-cruise travel.
        g->ship_speed_z -= GAME_BOOST_COAST_DECEL * dt;
        if (g->ship_speed_z <= g->ship_base_speed_z) {
            g->ship_speed_z = g->ship_base_speed_z;
            g->boost_phase  = BOOST_IDLE;
        }
    } else {
        bool  const scraping = g->scrape_left || g->scrape_right;
        float const target   = scraping
                                  ? (g->ship_base_speed_z * SCRAPE_SPEED_FRAC)
                                  : g->ship_base_speed_z;
        if (g->ship_speed_z > target) {
            float const max_step = SCRAPE_DECEL * dt;
            g->ship_speed_z -= max_step;
            if (g->ship_speed_z < target) g->ship_speed_z = target;
        } else if (g->ship_speed_z < target) {
            float const max_step = SPEED_RECOVERY * dt;
            g->ship_speed_z += max_step;
            if (g->ship_speed_z > target) g->ship_speed_z = target;
        }
    }

    // --- Scoring + distance integration ----------------------------------
    // Three streams contribute to `score`, all scaled by the
    // current multiplier:
    //   1. Distance (per-frame `dz × multiplier × DISTANCE_FACTOR`
    //      here) — the always-on income that keeps the score
    //      climbing even when no pickups are in reach. Factor is
    //      0.1 so the baseline stays subordinate to pickup bumps
    //      and Tris/boosters feel like real events instead of
    //      rounding error on top of the distance counter.
    //   2. Tri pickups (`GAME_SCORE_TRI × multiplier`) — bonus
    //      awarded inside `game_collide`'s Tri-collect branch.
    //   3. Booster pickups (`GAME_SCORE_BOOSTER × multiplier`)
    //      — bonus awarded in the same place for the booster.
    // `multiplier_max` is bumped where the multiplier itself
    // increases (Tri-collect on every 5th pickup), not here.
    {
        float const dz = g->ship_speed_z * dt;
        if (dz > 0.0f) {
            g->distance_traveled += (double)dz;
            g->score             += (double)dz * (double)g->multiplier
                                    * (double)GAME_SCORE_DISTANCE_FACTOR;
        }
    }

    // Run ends when the ship has fully coasted to a stop. The
    // floor logic still draws the final frame at zero speed before
    // the caller flips to GAME_OVER.
    return g->ship_speed_z <= 0.0f;
}

// --- Drawing ------------------------------------------------------------------

// Multiply each channel of an ARGB color by `scale` (0..1).
// Alpha kept intact. Saturates the result implicitly because the
// inputs are already in [0, 255] and scale ≤ 1.
static inline pax_col_t dim_argb(pax_col_t col, float scale) {
    uint32_t const a = (col >> 24) & 0xFF;
    uint32_t const r = (uint32_t)((float)((col >> 16) & 0xFF) * scale);
    uint32_t const g = (uint32_t)((float)((col >>  8) & 0xFF) * scale);
    uint32_t const b = (uint32_t)((float)((col >>  0) & 0xFF) * scale);
    return (a << 24) | (r << 16) | (g << 8) | b;
}

void game_draw_ship(pax_buf_t* fb, game_state_t const* g) {
    float const angle = g->bank * MAX_BANK_RAD;
    float const c     = cosf(angle);
    float const s     = sinf(angle);

    pax_vec2f screen[SHIP_VERT_COUNT];
    for (size_t i = 0; i < SHIP_VERT_COUNT; i++) {
        ship_vert_t const* v = &ship_verts[i];
        float const lx = v->x * c + v->y * s;
        float const ly = -v->x * s + v->y * c;
        float const wx = lx + g->ship_x_world;
        float const wy = ly + SHIP_BASE_Y + g->ship_y;
        float const wz = v->z + SHIP_Z_PLANE;
        render_project(wx, wy, wz, &screen[i].x, &screen[i].y);
    }

    // Pre-dim every ship colour once if the ship is in shadow —
    // 30% darker face + ridge fills. Far cheaper than per-pixel
    // attenuation, and works whether faces stay on PAX or move
    // to direct_565 later. Keeping the originals in locals so
    // the compiler can hoist either branch.
    pax_col_t       belly      = SHIP_BELLY_COLOR;
    pax_col_t       roof_left  = SHIP_ROOF_LEFT_COLOR;
    pax_col_t       roof_right = SHIP_ROOF_RIGHT_COLOR;
    pax_col_t       ridge      = SHIP_RIDGE_COLOR;
    if (g->in_shadow) {
        belly      = dim_argb(belly,      GAME_SHIP_SHADOW_TINT);
        roof_left  = dim_argb(roof_left,  GAME_SHIP_SHADOW_TINT);
        roof_right = dim_argb(roof_right, GAME_SHIP_SHADOW_TINT);
        ridge      = dim_argb(ridge,      GAME_SHIP_SHADOW_TINT);
    }

    for (size_t i = 0; i < SHIP_TRI_COUNT; i++) {
        ship_tri_t const* t   = &ship_tris[i];
        pax_col_t         col = belly;
        switch (t->face) {
            case SHIP_FACE_ROOF_LEFT:  col = roof_left;  break;
            case SHIP_FACE_ROOF_RIGHT: col = roof_right; break;
            case SHIP_FACE_BELLY:      col = belly;      break;
        }
        pax_simple_tri(fb, col,
                       screen[t->a].x, screen[t->a].y,
                       screen[t->b].x, screen[t->b].y,
                       screen[t->c].x, screen[t->c].y);
    }

    for (size_t i = 0; i < SHIP_OUTLINE_COUNT; i++) {
        uint8_t const a = ship_outline_edges[i][0];
        uint8_t const b = ship_outline_edges[i][1];
        pax_simple_line(fb, ridge,
                        screen[a].x, screen[a].y,
                        screen[b].x, screen[b].y);
    }
}

void game_draw_sparks(pax_buf_t* fb, game_state_t const* g) {
    if (g->scrape_left)  draw_wingtip_burst(fb, g, -1);
    if (g->scrape_right) draw_wingtip_burst(fb, g, +1);
}

// --- Crash explosion ----------------------------------------------------------

// Per-spark life — the longest-lived spark (≈0.6 s) outlasts the
// ~0.5 s crash SFX slightly so the shower covers the whole sound.
#define CRASH_SPARK_LIFE_MIN    0.30f   // s
#define CRASH_SPARK_LIFE_MAX    0.60f   // s
// Outward speed range, screen pixels/s — a wide spread so the burst
// reads as a chaotic shower rather than a uniform ring.
#define CRASH_SPARK_SPEED_MIN   120.0f
#define CRASH_SPARK_SPEED_MAX   540.0f
// Downward pull on the streaks (px/s²) so they arc like debris.
#define CRASH_SPARK_GRAVITY     430.0f
// Streak length at full life, pixels (shrinks as the spark fades).
#define CRASH_SPARK_LEN         10.0f

void game_crash_burst(game_state_t* g) {
    // Origin: the ship's centre projected to the screen. After this
    // the ship is no longer drawn — the sparks stand in for it.
    float ox, oy;
    render_project(g->ship_x_world, SHIP_BASE_Y + g->ship_y, SHIP_Z_PLANE,
                   &ox, &oy);

    for (int i = 0; i < CRASH_SPARK_COUNT; i++) {
        crash_spark_t* p   = &g->crash_sparks[i];
        float const    ang = spark_rand() * 6.28318531f;
        float const    spd = CRASH_SPARK_SPEED_MIN
                           + spark_rand() * (CRASH_SPARK_SPEED_MAX - CRASH_SPARK_SPEED_MIN);
        p->x        = ox;
        p->y        = oy;
        p->vx       = cosf(ang) * spd;
        p->vy       = sinf(ang) * spd;
        p->life_max = CRASH_SPARK_LIFE_MIN
                    + spark_rand() * (CRASH_SPARK_LIFE_MAX - CRASH_SPARK_LIFE_MIN);
        p->life     = p->life_max;
    }
}

bool game_crash_tick(game_state_t* g, float dt) {
    bool any_alive = false;
    for (int i = 0; i < CRASH_SPARK_COUNT; i++) {
        crash_spark_t* p = &g->crash_sparks[i];
        if (p->life <= 0.0f) continue;
        p->life -= dt;
        if (p->life <= 0.0f) { p->life = 0.0f; continue; }
        p->vy += CRASH_SPARK_GRAVITY * dt;
        p->x  += p->vx * dt;
        p->y  += p->vy * dt;
        any_alive = true;
    }
    return any_alive;
}

void game_draw_crash_sparks(pax_buf_t* fb, game_state_t const* g) {
    for (int i = 0; i < CRASH_SPARK_COUNT; i++) {
        crash_spark_t const* p = &g->crash_sparks[i];
        if (p->life <= 0.0f) continue;

        // Fade ratio 1 (fresh) → 0 (spent): the streak shrinks and
        // its colour cools from hot yellow-white to a red ember.
        float const f = (p->life_max > 0.0f) ? (p->life / p->life_max) : 0.0f;

        // Streak trails *behind* the velocity vector.
        float const sp = sqrtf(p->vx * p->vx + p->vy * p->vy);
        float       ux = 0.0f;
        float       uy = -1.0f;
        if (sp > 1.0f) { ux = p->vx / sp; uy = p->vy / sp; }
        float const tail = CRASH_SPARK_LEN * (0.35f + 0.65f * f);
        float const ex   = p->x - ux * tail;
        float const ey   = p->y - uy * tail;

        int const gc = (int)(0x20 + 0xC0 * f);   // green channel: 0x20..0xE0
        int const bc = (int)(0x10 + 0x60 * f);   // blue  channel: 0x10..0x70
        uint32_t const col = 0xFFFF0000u
                           | ((uint32_t)gc << 8) | (uint32_t)bc;
        pax_simple_line(fb, col, p->x, p->y, ex, ey);
    }
}
