#pragma once

#include <stdalign.h>
#include <stdbool.h>
#include <stdint.h>

#include "pax_gfx.h"

// Forward declarations so the callback typedefs don't drag world.h /
// game.h in via the header. Both structs get tagged definitions
// elsewhere (world_state_s in world.h, game_state_s in game.h).
struct world_state_s;
struct game_state_s;

// Obstacle kinds. Kept as a tag for save/inspection, default
// collision/render dispatch, and shadow-eligibility. Per-object
// callbacks (see below) override the dispatch when non-NULL.
typedef enum {
    OBSTACLE_KIND_CUBE = 0,        // pixel cubes, big blocks, gateway slabs — head-on fatal
    OBSTACLE_KIND_WALL,            // side walls — scrape-only
    OBSTACLE_KIND_PICKUP_TRI,      // Phase 6: collected, bumps multiplier
    OBSTACLE_KIND_PICKUP_BOOST,    // Phase 5: collected, fires the boost state machine
    OBSTACLE_KIND_PICKUP_JUMP,     // Phase 9
    OBSTACLE_KIND_PICKUP_SHIELD,   // Phase 9
    OBSTACLE_KIND_RAMP,            // future: contact triggers a jump
} obstacle_kind_t;

// Collision response classification. Returned by the per-object
// collide callback (or the default-dispatch helper) so the actual
// push-out / scrape-flag / run-end work stays centralised in
// game_collide.
typedef enum {
    OBSTACLE_HIT_IGNORE  = 0,  // pickup consumed, or other non-blocking contact
    OBSTACLE_HIT_SCRAPE  = 1,  // lateral push-out + set scrape flag
    OBSTACLE_HIT_HEAD_ON = 2,  // run-ending impact
} obstacle_hit_result_t;

typedef struct obstacle_s obstacle_t;

// Per-object callback table. Every callback is optional — NULL means
// "use the default kind-dispatched implementation". Wiring a callback
// lets an object override one aspect of its behaviour (e.g. a
// strafing drone with custom physics, a billboard with custom draw)
// without forcing it to reimplement the rest.

// Called once per frame inside the world-advance pool sweep, after
// the global z-advance and before collision. The obstacle's own
// `z_world` is already the camera-relative distance to the ship
// (camera/ship origin is at z=0), so callbacks don't get a separate
// ship_z. `cam_x` is the live lateral camera position, useful for
// objects that track the ship horizontally. `w` is the live world
// (PRNG, pool) so the callback can spawn children or read shared
// state.
typedef void (*obstacle_physics_fn)(obstacle_t* o,
                                    struct world_state_s* w,
                                    float dt, float cam_x);

// Called on despawn (object went past the camera, or was deactivated
// by collection). Use to free `internal` heap allocations if any —
// the inline scratch buffer below doesn't need cleanup, this is for
// anything else the object reached out to.
typedef void (*obstacle_cleanup_fn)(obstacle_t* o);

// Called when game_collide detects overlap with the ship. Return
// classifies the contact; the caller mutates ship state (push-out,
// scrape flag, head-on) accordingly. The callback itself may also
// mutate the game state (e.g. a pickup callback bumps a counter and
// deactivates the obstacle); the return is just the contact type.
typedef obstacle_hit_result_t (*obstacle_collide_fn)(obstacle_t* o,
                                                    struct game_state_s* g,
                                                    bool came_from_ahead);

// Called from render_obstacles inside the painter's-algorithm loop.
// Receives the framebuffer pointer and camera x. Default dispatch
// renders cubes / pyramids per kind; this overrides for objects with
// non-standard geometry.
typedef void (*obstacle_draw_fn)(pax_buf_t* fb, obstacle_t const* o, float cam_x);

// Called from render_shadows. Receives sun_y in addition to fb /
// cam_x so the callback can compute the shadow's z extent the same
// way the default does. Default dispatch draws trapezoid quads for
// cube-kind objects only; this overrides for objects that need a
// different shadow shape (or none at all — provide a callback that
// returns immediately).
typedef void (*obstacle_shadow_fn)(pax_buf_t* fb, obstacle_t const* o,
                                   float cam_x, float sun_y);

struct obstacle_s {
    // --- Identity / geometry ------------------------------------
    obstacle_kind_t kind;
    float    x_world;
    float    z_world;
    float    half_w;
    float    half_d;
    float    height;
    // y_base = vertical offset of the cube's bottom face above the
    // ground plane. Default 0 → standard ground-level cube. Elevated
    // objects (pillars sitting on top of walls, bridge spans, future
    // floating decals) set this so the renderer puts the geometry
    // at the right altitude. The cube's y range is
    // [y_base, y_base + height]. Default render reads this; custom
    // draw callbacks may override.
    float    y_base;

    // --- Standard colour set (read by the default renderer) -----
    uint32_t front_color;
    uint32_t side_color;
    uint32_t top_color;
    uint32_t outline_color;

    bool     active;

    // --- Per-object behaviour overrides -------------------------
    // All optional; NULL falls back to the default dispatch.
    obstacle_physics_fn physics;
    obstacle_cleanup_fn cleanup;
    obstacle_collide_fn collide;
    obstacle_draw_fn    draw;
    obstacle_shadow_fn  shadow;

    // --- Per-object private state -------------------------------
    // 128 bytes, 16-byte aligned. Object types overlay their own
    // struct on this buffer via `(my_state_t*)o->scratch` so the
    // layout is private to that object type. No malloc, no
    // fragmentation. If 128 B isn't enough, allocate externally and
    // store the pointer at the head of scratch — but cleanup() must
    // then free it.
    alignas(16) uint8_t scratch[128];
};

// Find a free pool slot in `w->obstacles[]`, populate the basic
// fields (kind, position, dimensions, colours), zero the scratch
// buffer and all callbacks, and mark active. Returns the freshly
// activated slot, or NULL on a full pool (drops the spawn silently).
//
// After the call, the caller can install callbacks and / or
// populate scratch as needed:
//
//   obstacle_t* o = obstacle_spawn(w, KIND, x, z, hw, hd, h, fc, sc, tc, oc);
//   if (o) {
//       o->physics = my_physics;
//       my_state_t* s = (my_state_t*)o->scratch;
//       s->target_x = …;
//   }
obstacle_t* obstacle_spawn(struct world_state_s* w, obstacle_kind_t kind,
                           float x, float z,
                           float half_w, float half_d, float height,
                           uint32_t front_color, uint32_t side_color,
                           uint32_t top_color,  uint32_t outline_color);

// Deactivate an obstacle, running its cleanup callback (if any)
// first. Use this for any explicit deactivation (pickup collection,
// gameplay event) so cleanup runs consistently. The world-advance
// despawn pass also routes through here.
void obstacle_despawn(obstacle_t* o);

// No public default-dispatch helpers — the "default" behaviour lives
// inline at each NULL-check site (game_collide handles collide,
// render_obstacles handles draw, render_shadows handles shadow).
// Object modules that want the standard behaviour for their kind
// just leave the callback NULL.
