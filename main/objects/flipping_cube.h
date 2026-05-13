#pragma once

#include <stdint.h>

#include "obstacle.h"
#include "objects/cube.h"   // pulls in CUBE_BIG_* dimensions

struct world_state_s;

// Flipping cube — a big-block-sized obstacle that rolls 90° around
// one of its bottom z-running edges as it approaches the camera.
// Two visual sub-types share this implementation:
//   direction = -1 : "left-roll"  — pivots on its bottom-left edge,
//                    body tips to the LEFT.  Edge outline is red.
//   direction = +1 : "right-roll" — pivots on its bottom-right edge,
//                    body tips to the RIGHT. Edge outline is green.
//
// The rotation is driven entirely by world_z position (so it's
// frame-rate independent and re-spawning the same world reproduces
// the animation deterministically): rotation starts at
// FLIPPING_CUBE_ROLL_START_Z and finishes at FLIPPING_CUBE_ROLL_END_Z.
// Outside that band the cube is either fully upright (z above start)
// or fully on its side (z below end).
//
// Geometry matches the big-block cube exactly (CUBE_BIG_HALF_W /
// CUBE_BIG_HALF_D / CUBE_BIG_HEIGHT) so the upright body is
// indistinguishable in size from area_big_blocks's content. Because
// the body is taller than it is wide, a full 90° flip ends with the
// cube lying on its side: footprint 2 × CUBE_BIG_HEIGHT wide
// (in the roll direction), height 2 × CUBE_BIG_HALF_W tall — i.e.
// shorter and wider than it started. Collision and shadow use the
// current rotated AABB, updated each frame by the physics callback.

#define FLIPPING_CUBE_HALF_W   CUBE_BIG_HALF_W
#define FLIPPING_CUBE_HALF_D   CUBE_BIG_HALF_D
#define FLIPPING_CUBE_HEIGHT   CUBE_BIG_HEIGHT

// World-z thresholds for the roll animation. Tuned so the player
// sees the rotation visibly happen across the front half of their
// view (cube is about waist-height on screen when rotation begins,
// landed and stationary by the time it's a few units in front of
// the ship's collision box).
#define FLIPPING_CUBE_ROLL_START_Z   50.0f
#define FLIPPING_CUBE_ROLL_END_Z      8.0f

// Spawn a flipping cube. `direction` MUST be -1 (left-roll) or
// +1 (right-roll); other values are not validated and produce
// undefined orientations. `x` is the world-x of the upright cube's
// CENTER (not the pivot edge); the pivot is automatically chosen on
// the side that matches `direction` so the cube tips that way.
obstacle_t* flipping_cube_spawn(struct world_state_s* w,
                                float x, float z, int direction);
