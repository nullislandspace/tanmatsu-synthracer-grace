#include "objects/rest_pillar.h"

#include "objects/wall.h"   // WALL_X_LEFT / WALL_X_RIGHT / WALL_HALF_W / WALL_HEIGHT
#include "world.h"

// Green palette — distinct from the magenta side walls, amber gate
// slabs and concrete-grey bridges, so the posts read instantly as
// "rest area" markers. Cyan wireframe matches the rest of the
// world's hard-surface objects.
#define REST_PILLAR_FRONT_COLOR    0xFF2FBF2Fu
#define REST_PILLAR_SIDE_COLOR     0xFF1C781Cu
#define REST_PILLAR_TOP_COLOR      0xFF45E045u
#define REST_PILLAR_OUTLINE_COLOR  0xFF31FBFBu

static void spawn_post(world_state_t* w, float x, float z) {
    obstacle_t* const p = obstacle_spawn(
        w, OBSTACLE_KIND_CUBE,
        x, z,
        WALL_HALF_W, REST_PILLAR_HALF_D, REST_PILLAR_HEIGHT,
        REST_PILLAR_FRONT_COLOR, REST_PILLAR_SIDE_COLOR,
        REST_PILLAR_TOP_COLOR,   REST_PILLAR_OUTLINE_COLOR);
    // Sits on top of the side wall, like the bridge pillars — lift
    // the base to the wall's top face so it doesn't intersect the
    // wall. The default cube renderer honours y_base, so no custom
    // draw is needed. The post sits at the wall x with the wall's
    // own footprint, fully outside the ship's reachable x range, so
    // it never collides (same as the bridge pillars — no collide
    // override required).
    if (p) p->y_base = WALL_HEIGHT;
}

void rest_pillar_pair_spawn(world_state_t* w, float z) {
    // Nudged 0.01 u toward the camera so painter's sort places the
    // post strictly after any co-located wall segment (same trick
    // the bridge pillars use).
    float const post_z = z - 0.01f;
    spawn_post(w, WALL_X_LEFT,  post_z);
    spawn_post(w, WALL_X_RIGHT, post_z);
}
