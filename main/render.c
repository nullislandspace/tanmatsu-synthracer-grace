#include "render.h"

#include "shapes/pax_tris.h"

// Synthwave palette for obstacles. The front face is full magenta;
// the side face is a halved variant so adjacent faces read as
// differently-lit and the cube's depth is legible. The top face is
// lighter (sunlit) and only drawn when the camera is above the cube
// top, which never happens at the current OBSTACLE_HEIGHT=2 with
// cam_y=1, but the code path is in place for future shorter types.
#define OBSTACLE_FRONT_COLOR   0xFFF71FF1u
#define OBSTACLE_SIDE_COLOR    0xFF7B1078u
#define OBSTACLE_TOP_COLOR     0xFFFDAFECu
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

        // Cube footprint: square in plan, so depth half-extent equals
        // lateral half-extent. z_world is treated as the cube's centre
        // along z; front face sits at z_world - half_d, back at
        // z_world + half_d. Clamp the front to a small positive z so a
        // cube straddling the near plane (centre near the despawn
        // threshold of 0.6 with half_d≈0.4) doesn't blow up the
        // projection.
        float const half_d = o->half_w;
        float const xL     = o->x_world - o->half_w;
        float const xR     = o->x_world + o->half_w;
        float       zF     = o->z_world - half_d;
        float const zB     = o->z_world + half_d;
        float const yT     = o->height;
        if (zF < 0.05f) zF = 0.05f;

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
            pax_simple_tri(fb, OBSTACLE_SIDE_COLOR, sx_LBF, sy_LBF, sx_LTF, sy_LTF, sx_LTB, sy_LTB);
            pax_simple_tri(fb, OBSTACLE_SIDE_COLOR, sx_LBF, sy_LBF, sx_LTB, sy_LTB, sx_LBB, sy_LBB);
        } else if (show_right) {
            pax_simple_tri(fb, OBSTACLE_SIDE_COLOR, sx_RBF, sy_RBF, sx_RTF, sy_RTF, sx_RTB, sy_RTB);
            pax_simple_tri(fb, OBSTACLE_SIDE_COLOR, sx_RBF, sy_RBF, sx_RTB, sy_RTB, sx_RBB, sy_RBB);
        }

        if (show_top) {
            pax_simple_tri(fb, OBSTACLE_TOP_COLOR, sx_LTF, sy_LTF, sx_RTF, sy_RTF, sx_RTB, sy_RTB);
            pax_simple_tri(fb, OBSTACLE_TOP_COLOR, sx_LTF, sy_LTF, sx_RTB, sy_RTB, sx_LTB, sy_LTB);
        }

        // Front face (always visible for obstacles in front of the
        // camera, which all active obstacles are by construction).
        pax_simple_tri(fb, OBSTACLE_FRONT_COLOR, sx_LBF, sy_LBF, sx_RBF, sy_RBF, sx_RTF, sy_RTF);
        pax_simple_tri(fb, OBSTACLE_FRONT_COLOR, sx_LBF, sy_LBF, sx_RTF, sy_RTF, sx_LTF, sy_LTF);

        // Cyan wireframe over the visible silhouette: front face
        // edges plus the back-edge contour of the visible side and
        // (when applicable) top, so the cube reads as a 3D shape
        // even when the side colour is hard to distinguish from the
        // backdrop.
        pax_simple_line(fb, OBSTACLE_OUTLINE_COLOR, sx_LBF, sy_LBF, sx_RBF, sy_RBF);
        pax_simple_line(fb, OBSTACLE_OUTLINE_COLOR, sx_LTF, sy_LTF, sx_RTF, sy_RTF);
        pax_simple_line(fb, OBSTACLE_OUTLINE_COLOR, sx_LBF, sy_LBF, sx_LTF, sy_LTF);
        pax_simple_line(fb, OBSTACLE_OUTLINE_COLOR, sx_RBF, sy_RBF, sx_RTF, sy_RTF);
        if (show_left) {
            pax_simple_line(fb, OBSTACLE_OUTLINE_COLOR, sx_LBF, sy_LBF, sx_LBB, sy_LBB);
            pax_simple_line(fb, OBSTACLE_OUTLINE_COLOR, sx_LTF, sy_LTF, sx_LTB, sy_LTB);
            pax_simple_line(fb, OBSTACLE_OUTLINE_COLOR, sx_LBB, sy_LBB, sx_LTB, sy_LTB);
        } else if (show_right) {
            pax_simple_line(fb, OBSTACLE_OUTLINE_COLOR, sx_RBF, sy_RBF, sx_RBB, sy_RBB);
            pax_simple_line(fb, OBSTACLE_OUTLINE_COLOR, sx_RTF, sy_RTF, sx_RTB, sy_RTB);
            pax_simple_line(fb, OBSTACLE_OUTLINE_COLOR, sx_RBB, sy_RBB, sx_RTB, sy_RTB);
        }
        if (show_top) {
            pax_simple_line(fb, OBSTACLE_OUTLINE_COLOR, sx_LTB, sy_LTB, sx_RTB, sy_RTB);
            if (!show_left)  pax_simple_line(fb, OBSTACLE_OUTLINE_COLOR, sx_LTF, sy_LTF, sx_LTB, sy_LTB);
            if (!show_right) pax_simple_line(fb, OBSTACLE_OUTLINE_COLOR, sx_RTF, sy_RTF, sx_RTB, sy_RTB);
        }
    }
}
