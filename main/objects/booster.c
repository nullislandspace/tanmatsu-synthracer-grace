#include "objects/booster.h"

#include "magicnumbers.h"
#include "world.h"

obstacle_t* booster_spawn(world_state_t* w) {
    float const x = (world_frand(&w->stage_prng) * 2.0f - 1.0f) * TRACK_HALF_WIDTH;
    return booster_spawn_at(w, x, WORLD_Z_FAR_SPAWN);
}

obstacle_t* booster_spawn_at(world_state_t* w, float x, float z) {
    return obstacle_spawn(w, OBSTACLE_KIND_PICKUP_BOOST,
                          x, z,
                          GAME_BOOSTER_HALF_W, GAME_BOOSTER_HALF_W, GAME_BOOSTER_HEIGHT,
                          GAME_BOOSTER_FRONT_COLOR, GAME_BOOSTER_SIDE_COLOR,
                          /* top_color */ GAME_BOOSTER_FRONT_COLOR,
                          GAME_BOOSTER_OUTLINE_COLOR);
}
