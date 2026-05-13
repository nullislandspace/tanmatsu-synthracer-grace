#include "objects/tri.h"

#include "magicnumbers.h"
#include "obstacle.h"
#include "world.h"

obstacle_t* tri_spawn_at(world_state_t* w, float x, float z) {
    // Same shape/size constants as the original booster pyramid;
    // the cyan-blue palette is what makes it visually distinct
    // from the booster (now a rotating icosahedron) at a glance.
    // Top colour reuses the front so the pyramid's apex blends
    // into the brightest face — same convention the original
    // booster pyramid used.
    return obstacle_spawn(w, OBSTACLE_KIND_PICKUP_TRI,
                          x, z,
                          GAME_TRI_HALF_W, GAME_TRI_HALF_W, GAME_TRI_HEIGHT,
                          GAME_TRI_FRONT_COLOR,
                          GAME_TRI_SIDE_COLOR,
                          /* top_color */ GAME_TRI_FRONT_COLOR,
                          GAME_TRI_OUTLINE_COLOR);
}
