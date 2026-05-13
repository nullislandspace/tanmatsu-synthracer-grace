#pragma once

#include "obstacle.h"

struct world_state_s;

// Spawn side-wall segments out to the far plane on one side of the
// track. `far_cursor` is the camera-relative z where the *next* far
// segment should land — the caller decrements it by `dz` each frame
// before calling, and this function spawns segments + advances the
// cursor until it sits beyond WORLD_Z_FAR_SPAWN. wall_x is the x
// position of the wall's centre line (±(TRACK_HALF_WIDTH + half_w)).
void wall_top_up(struct world_state_s* w, float* far_cursor, float wall_x);

// Wall geometry / palette. Exported because the world owns the
// initial cursor placement (at half a segment so joints align with
// the floor grid), and because WALL_X_RIGHT / WALL_X_LEFT are
// driven from TRACK_HALF_WIDTH and need to stay in sync.
#define WALL_SEGMENT_LEN     3.0f
#define WALL_SEGMENT_HALF_D  (WALL_SEGMENT_LEN * 0.5f)
#define WALL_HALF_W          0.5f
#define WALL_HEIGHT          (2.0f / 3.0f)
#define WALL_X_RIGHT         (TRACK_HALF_WIDTH + WALL_HALF_W)
#define WALL_X_LEFT          (-(TRACK_HALF_WIDTH + WALL_HALF_W))
