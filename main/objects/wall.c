#include "objects/wall.h"

#include "world.h"

#define WALL_FRONT_COLOR     0xFF8C1A8Cu
#define WALL_SIDE_COLOR      0xFF551154u
#define WALL_TOP_COLOR       0xFFD040C5u
#define WALL_OUTLINE_COLOR   0xFF31FBFBu

void wall_top_up(world_state_t* w, float* far_cursor, float wall_x) {
    while (*far_cursor < WORLD_Z_FAR_SPAWN) {
        obstacle_spawn(w, OBSTACLE_KIND_WALL,
                       wall_x, *far_cursor,
                       WALL_HALF_W, WALL_SEGMENT_HALF_D, WALL_HEIGHT,
                       WALL_FRONT_COLOR, WALL_SIDE_COLOR,
                       WALL_TOP_COLOR,   WALL_OUTLINE_COLOR);
        *far_cursor += WALL_SEGMENT_LEN;
    }
}
