// Tri pickup (Phase 6 — Tris + multiplier).
//
// Geometry is a copy of the original booster shape: a square-based
// pyramid sized to a pixel-cube footprint, apex at base-width
// above the ground plane. Distinct colour palette (cyan-blue) so
// the player can read it at a glance against the booster's
// icosahedron.
//
// Collect rule (in `game_collide`): increment `pickups_tri`, bump
// score by `GAME_SCORE_TRI × multiplier`, fire the ascending
// pentatonic plink SFX (slot index `(pickups_tri - 1) % 5`), and
// if `pickups_tri % 5 == 0` tick the multiplier up by one. The
// HUD multiplier panel reads the same `pickups_tri % 5` for its
// 4-slot progress display.
//
// Spawn is owned by the per-area generators (`main/areas/*.c`)
// which apply per-area density rules — see DEVELOPMENT.md's
// 2026-05-14 Phase 6 decisions-log entry for the rules.

#pragma once

struct world_state_s;
struct obstacle_s;

// Spawn one Tri at (x, z) on the playfield. Returns the spawned
// obstacle, or NULL if the pool is full (drop silently — Tris
// are bonus pickups, not mandatory path elements). Caller chooses
// the position; this helper does no clamping (the area generator
// already enforces playfield bounds + collision-avoidance).
struct obstacle_s* tri_spawn_at(struct world_state_s* w, float x, float z);
