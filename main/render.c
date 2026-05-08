#include "render.h"

#include "shapes/pax_tris.h"

// Per-obstacle dimensions and colours come from the obstacle_t
// itself (world.c sets them at spawn time) so the renderer treats
// every entry — dynamic obstacles, side-wall segments, future
// pickups — uniformly. The cube has a square footprint when
// half_w == half_d; non-square footprints (e.g. wall segments
// running along z) work without any special-case code.
//
// Near-plane clipping. When the camera moves into a long obstacle's
// z range (the front edge passes the camera before the back does)
// the projection of the front face heads to infinity. We don't try
// to render anything closer than NEAR_CLIP_Z; if the front edge is
// past the clip plane we drop the front face entirely and clip the
// side and top faces' front edges back to NEAR_CLIP_Z. The whole
// cube is skipped if the back edge is also past the clip — by then
// the obstacle's despawn condition will fire on the next frame.
#define NEAR_CLIP_Z 0.5f

void render_project(float x_w, float y_w, float z_w, float cam_x, float* out_sx, float* out_sy) {
    if (z_w < 0.01f) z_w = 0.01f;  // guard against /0 if a near-clip slips through
    float const inv_z = 1.0f / z_w;
    *out_sx = RENDER_HALF_W + RENDER_FOCAL_LEN * (x_w - cam_x) * inv_z;
    *out_sy = RENDER_HORIZON_Y - RENDER_FOCAL_LEN * (y_w - RENDER_CAM_Y) * inv_z;
}

void render_obstacles(pax_buf_t* fb, world_state_t const* w, float cam_x) {
    // Build an index list over the active subset, then sort it
    // descending by z so painter's algorithm draws far → near. n is
    // bounded by the pool size (64) so insertion sort is plenty. We
    // sort by the obstacle's centre z_world; obstacles rarely overlap
    // in z so this is a fine proxy for the cube's actual extent.
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

    for (int k = 0; k < n; k++) {
        obstacle_t const* o = &w->obstacles[idx[k]];

        // Cube extents come straight from the obstacle. z_world is
        // the centre along z; front face sits at zF_raw, back at
        // zB. If the back edge is already past the near clip the
        // whole cube has nothing to draw — bail. Otherwise clip the
        // front edge to NEAR_CLIP_Z; if zF_raw was further than the
        // clip we keep the real front face, otherwise we drop the
        // front face and the side/top faces use the clipped near
        // edge instead of the geometric front.
        float const xL     = o->x_world - o->half_w;
        float const xR     = o->x_world + o->half_w;
        float const zF_raw = o->z_world - o->half_d;
        float const zB     = o->z_world + o->half_d;
        float const yT     = o->height;
        if (zB < NEAR_CLIP_Z) continue;
        bool  const front_visible = (zF_raw >= NEAR_CLIP_Z);
        float const zF            = front_visible ? zF_raw : NEAR_CLIP_Z;

        // Project the 6 corners we actually need (front face + the
        // back edge of the visible side + the back-top edge for the
        // top face). The two unused back-bottom corners (LBB, RBB)
        // would only be needed if we drew the bottom face, which is
        // never visible (camera is above ground).
        float sx_LBF, sy_LBF, sx_RBF, sy_RBF, sx_LTF, sy_LTF, sx_RTF, sy_RTF;
        float sx_LTB, sy_LTB, sx_RTB, sy_RTB, sx_LBB, sy_LBB, sx_RBB, sy_RBB;
        render_project(xL, 0.0f, zF, cam_x, &sx_LBF, &sy_LBF);
        render_project(xR, 0.0f, zF, cam_x, &sx_RBF, &sy_RBF);
        render_project(xL, yT,   zF, cam_x, &sx_LTF, &sy_LTF);
        render_project(xR, yT,   zF, cam_x, &sx_RTF, &sy_RTF);
        render_project(xL, yT,   zB, cam_x, &sx_LTB, &sy_LTB);
        render_project(xR, yT,   zB, cam_x, &sx_RTB, &sy_RTB);
        render_project(xL, 0.0f, zB, cam_x, &sx_LBB, &sy_LBB);
        render_project(xR, 0.0f, zB, cam_x, &sx_RBB, &sy_RBB);

        // Visible-face selection. A face is visible when the camera
        // is on the side its outward normal points to.
        bool const show_left  = cam_x < xL;             // camera left of the cube → left face visible
        bool const show_right = cam_x > xR;             // camera right of the cube → right face visible
        bool const show_top   = RENDER_CAM_Y > yT;      // camera above the cube → top face visible

        // Painter's order within the cube: side and top are at least
        // partially deeper than the front, so draw them first. The
        // front face overpaints any sliver of side/top that leaks
        // through to the front edge.
        if (show_left) {
            pax_simple_tri(fb, o->side_color, sx_LBF, sy_LBF, sx_LTF, sy_LTF, sx_LTB, sy_LTB);
            pax_simple_tri(fb, o->side_color, sx_LBF, sy_LBF, sx_LTB, sy_LTB, sx_LBB, sy_LBB);
        } else if (show_right) {
            pax_simple_tri(fb, o->side_color, sx_RBF, sy_RBF, sx_RTF, sy_RTF, sx_RTB, sy_RTB);
            pax_simple_tri(fb, o->side_color, sx_RBF, sy_RBF, sx_RTB, sy_RTB, sx_RBB, sy_RBB);
        }

        if (show_top) {
            pax_simple_tri(fb, o->top_color, sx_LTF, sy_LTF, sx_RTF, sy_RTF, sx_RTB, sy_RTB);
            pax_simple_tri(fb, o->top_color, sx_LTF, sy_LTF, sx_RTB, sy_RTB, sx_LTB, sy_LTB);
        }

        if (front_visible) {
            pax_simple_tri(fb, o->front_color, sx_LBF, sy_LBF, sx_RBF, sy_RBF, sx_RTF, sy_RTF);
            pax_simple_tri(fb, o->front_color, sx_LBF, sy_LBF, sx_RTF, sy_RTF, sx_LTF, sy_LTF);
        }

        // Cyan wireframe — each of the cube's 12 edges is drawn
        // exactly once, conditionally on whether it bounds any
        // visible face. Bottom and back faces are never visible
        // (camera is above ground and looking forward), so edges
        // belonging only to those are silently skipped. Listing
        // edge-by-edge avoids the bug where merging the front
        // face's left/right verticals into the side-face branches
        // would drop one of them whenever the camera saw only one
        // side of a tall pillar.
        if (front_visible)              pax_simple_line(fb, o->outline_color, sx_LBF, sy_LBF, sx_RBF, sy_RBF);
        if (front_visible || show_top)  pax_simple_line(fb, o->outline_color, sx_LTF, sy_LTF, sx_RTF, sy_RTF);
        if (front_visible || show_left) pax_simple_line(fb, o->outline_color, sx_LBF, sy_LBF, sx_LTF, sy_LTF);
        if (front_visible || show_right)pax_simple_line(fb, o->outline_color, sx_RBF, sy_RBF, sx_RTF, sy_RTF);
        if (show_top)                   pax_simple_line(fb, o->outline_color, sx_LTB, sy_LTB, sx_RTB, sy_RTB);
        if (show_top || show_left)      pax_simple_line(fb, o->outline_color, sx_LTF, sy_LTF, sx_LTB, sy_LTB);
        if (show_top || show_right)     pax_simple_line(fb, o->outline_color, sx_RTF, sy_RTF, sx_RTB, sy_RTB);
        if (show_left) {
            pax_simple_line(fb, o->outline_color, sx_LBF, sy_LBF, sx_LBB, sy_LBB);
            pax_simple_line(fb, o->outline_color, sx_LBB, sy_LBB, sx_LTB, sy_LTB);
        }
        if (show_right) {
            pax_simple_line(fb, o->outline_color, sx_RBF, sy_RBF, sx_RBB, sy_RBB);
            pax_simple_line(fb, o->outline_color, sx_RBB, sy_RBB, sx_RTB, sy_RTB);
        }
    }
}
