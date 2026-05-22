#pragma once

#include "obstacle.h"

struct world_state_s;

// A gantry sign that spans the track standing on the border walls, with
// an arbitrary text rendered across its panel. The mesh
// (objects/synthengine_sign_model.h, imported from openscad/synthengine3d.3mf)
// carries NO text — `text` is drawn at runtime as cheap Hershey vector
// strokes on the panel face, so the object is generic and reusable for
// any sign string (the AREA decides what to display).
//
// Collision: head-on crash only against the sign panel + holders (up high,
// y ~ 7.7-10.7); the pillars sit on the walls outside the ship's reach and
// the text is non-colliding. Spawns one obstacle at lateral centre, z, with
// its base on the wall tops. `text` (copied into the obstacle, so the caller
// need not keep the string alive) is rendered in `text_color` (ARGB) — the
// caller picks it so the sign is reusable for different signage. Returns the
// obstacle, or NULL if the pool is full.
obstacle_t* synthengine_sign_spawn(struct world_state_s* w, float z,
                                   char const* text, uint32_t text_color);
