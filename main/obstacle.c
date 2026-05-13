#include "obstacle.h"

#include <string.h>

#include "world.h"  // for world_state_t / WORLD_OBSTACLE_POOL_SIZE

obstacle_t* obstacle_spawn(world_state_t* w, obstacle_kind_t kind,
                           float x, float z,
                           float half_w, float half_d, float height,
                           uint32_t front_color, uint32_t side_color,
                           uint32_t top_color,  uint32_t outline_color) {
    for (int i = 0; i < WORLD_OBSTACLE_POOL_SIZE; i++) {
        obstacle_t* o = &w->obstacles[i];
        if (o->active) continue;
        // Zero everything so leftover callbacks / scratch from the
        // previous tenant don't leak into the fresh spawn. The
        // caller then sets any callbacks / scratch they want.
        memset(o, 0, sizeof(*o));
        o->kind          = kind;
        o->x_world       = x;
        o->z_world       = z;
        o->half_w        = half_w;
        o->half_d        = half_d;
        o->height        = height;
        o->front_color   = front_color;
        o->side_color    = side_color;
        o->top_color     = top_color;
        o->outline_color = outline_color;
        o->active        = true;
        return o;
    }
    return NULL;
}

void obstacle_despawn(obstacle_t* o) {
    if (!o || !o->active) return;
    if (o->cleanup) o->cleanup(o);
    o->active = false;
}
