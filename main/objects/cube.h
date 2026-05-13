#pragma once

#include "obstacle.h"

struct world_state_s;

// Pixel-field cube — small magenta dodge-or-die obstacles. Spawns
// at the far plane with random x across the track.
obstacle_t* cube_spawn_pixel(struct world_state_s* w);

// Same magenta pixel cube but at an explicit (x, z). Used by areas
// that decide placement themselves (e.g. dynamic_passage's clutter
// fill, which avoids one specific lane). No wall-clearance clamp —
// the caller is responsible for keeping the x inside the playfield.
obstacle_t* cube_spawn_pixel_at(struct world_state_s* w, float x, float z);

// Big-block cube — 2× the pixel cube laterally + along z, grey
// palette. Same collision behaviour (head-on fatal, scrape allowed
// on the trailing edge). Spawns at the far plane, random x.
obstacle_t* cube_spawn_big(struct world_state_s* w);

// Gateway slab — a CUBE-kind obstacle with caller-supplied
// dimensions / position / palette. Used by the gateways area to
// build the two slabs flanking each gap.
obstacle_t* cube_spawn_gate_slab(struct world_state_s* w,
                                 float x, float z, float half_w);

// Dimensions of the standard pixel cube (exported because shadow
// detection and a few helpers want the canonical half-width).
#define CUBE_PIXEL_HALF_W   0.4f
#define CUBE_PIXEL_HEIGHT   2.0f

// Big-block dimensions.
#define CUBE_BIG_HALF_W     (CUBE_PIXEL_HALF_W * 2.0f)
#define CUBE_BIG_HALF_D     (CUBE_PIXEL_HALF_W * 2.0f)
#define CUBE_BIG_HEIGHT     CUBE_PIXEL_HEIGHT

// Gateway slab depth + height (the slab's lateral half-width is
// caller-chosen — depends on the gap position).
#define CUBE_GATE_HALF_D    0.4f
#define CUBE_GATE_HEIGHT    CUBE_PIXEL_HEIGHT
