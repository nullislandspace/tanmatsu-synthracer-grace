#pragma once

#include "obstacle.h"

struct world_state_s;

// Concrete-grey bridge: two pillars rising from the side walls and a
// flat horizontal span connecting them across the playfield. The
// bridge is mostly visual — the ship flies under the span, no
// collision — and casts a wide rectangular shadow on the floor.
//
// `z` is the world-z of the bridge's centre. The caller is
// responsible for snapping it to a wall-segment-aligned position so
// the bridge sits cleanly on the wall grid; passing an unaligned z
// will work but produces a 1.5 u offset between the pillar and the
// wall-segment joints under it.
//
// Spawns 3 pool entries (left pillar, right pillar, span). On a
// full pool any of the three may fail to spawn — the caller doesn't
// need to handle this; the bridge just renders partially.
void bridge_spawn(struct world_state_s* w, float z);

// Bridge geometry. Matches the side-wall stride so the bridge
// occupies one wall-segment-wide z range exactly.
#define BRIDGE_DEPTH         3.0f       // = WALL_SEGMENT_LEN; one wall segment / floor stripe
#define BRIDGE_HALF_D        (BRIDGE_DEPTH * 0.5f)
// Pillars sit on top of the side walls (y_base = WALL_HEIGHT). The
// pillar's local height is added to that to get its top face.
#define BRIDGE_PILLAR_HEIGHT 3.0f
#define BRIDGE_SPAN_HEIGHT   0.5f       // flat slab between the pillars
// Span sits on top of the pillars: pillar_top_y = wall_top + pillar_height.
// WALL_HEIGHT is duplicated from wall.h's literal (2.0 / 3.0) because
// including wall.h here would pull TRACK_HALF_WIDTH which lives in
// world.h, creating an awkward include chain in this header.
#define BRIDGE_SPAN_Y_BASE   ((2.0f / 3.0f) + BRIDGE_PILLAR_HEIGHT)
