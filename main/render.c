#include "render.h"

#include "shapes/pax_tris.h"

// Synthwave palette for obstacles. Bright magenta against the cyan
// wireframe and yellow sun reads cleanly during fast scrolling.
#define OBSTACLE_FRONT_COLOR 0xFFF71FF1u
#define OBSTACLE_OUTLINE_COLOR 0xFF31FBFBu

void render_project(float x_w, float y_w, float z_w, float cam_x, float* out_sx, float* out_sy) {
    if (z_w < 0.01f) z_w = 0.01f;  // guard against /0 if a near-clip slips through
    float const inv_z = 1.0f / z_w;
    *out_sx = RENDER_HALF_W + RENDER_FOCAL_LEN * (x_w - cam_x) * inv_z;
    *out_sy = RENDER_HORIZON_Y - RENDER_FOCAL_LEN * (y_w - RENDER_CAM_Y) * inv_z;
}

void render_obstacles(pax_buf_t* fb, world_state_t const* w, float cam_x) {
    // Build an index list over the active subset, then sort it
    // descending by z so painter's algorithm draws far → near. n is
    // bounded by the pool size (64) so insertion sort is plenty.
    int idx[WORLD_OBSTACLE_POOL_SIZE];
    int n = 0;
    for (int i = 0; i < WORLD_OBSTACLE_POOL_SIZE; i++) {
        if (w->obstacles[i].active) idx[n++] = i;
    }
    for (int i = 1; i < n; i++) {
        int   k    = idx[i];
        float zk   = w->obstacles[k].z_world;
        int   j    = i - 1;
        while (j >= 0 && w->obstacles[idx[j]].z_world < zk) {
            idx[j + 1] = idx[j];
            j--;
        }
        idx[j + 1] = k;
    }

    // Project + draw front face as 2 triangles. All four corners share
    // the same z, so we factor out 1/z and reuse it across edges.
    for (int k = 0; k < n; k++) {
        obstacle_t const* o = &w->obstacles[idx[k]];
        float const z       = (o->z_world < 0.01f) ? 0.01f : o->z_world;
        float const inv_z   = 1.0f / z;
        float const fz      = RENDER_FOCAL_LEN * inv_z;
        float const sx_l    = RENDER_HALF_W + (o->x_world - o->half_w - cam_x) * fz;
        float const sx_r    = RENDER_HALF_W + (o->x_world + o->half_w - cam_x) * fz;
        float const sy_b    = RENDER_HORIZON_Y + RENDER_CAM_Y * fz;          // base sits on y=0
        float const sy_t    = RENDER_HORIZON_Y - (o->height - RENDER_CAM_Y) * fz;

        pax_simple_tri(fb, OBSTACLE_FRONT_COLOR, sx_l, sy_t, sx_r, sy_t, sx_l, sy_b);
        pax_simple_tri(fb, OBSTACLE_FRONT_COLOR, sx_r, sy_t, sx_r, sy_b, sx_l, sy_b);

        // Cyan outline so close obstacles read against the floor grid.
        pax_simple_line(fb, OBSTACLE_OUTLINE_COLOR, sx_l, sy_t, sx_r, sy_t);
        pax_simple_line(fb, OBSTACLE_OUTLINE_COLOR, sx_l, sy_b, sx_r, sy_b);
        pax_simple_line(fb, OBSTACLE_OUTLINE_COLOR, sx_l, sy_t, sx_l, sy_b);
        pax_simple_line(fb, OBSTACLE_OUTLINE_COLOR, sx_r, sy_t, sx_r, sy_b);
    }
}
