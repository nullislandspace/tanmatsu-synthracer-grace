#include "scene.h"

#include <math.h>
#include <string.h>

#include "direct_565.h"   // direct_565_logical_index, direct_565_pack
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "magicnumbers.h"
#include "render.h"        // render_camera, RENDER_* projection constants

static char const* TAG = "scene";

// --- Depth encoding -----------------------------------------------------------
//
// Depth is the reciprocal of world-z (1/z), which is the quantity
// that interpolates linearly in screen space under the pinhole
// projection. Near-clipped z is RENDER_NEAR_CLIP_Z (0.5), so 1/z
// peaks at 2.0; SCENE_DEPTH_SCALE maps that to ~64000, leaving the
// uint16 range comfortably unsaturated. Larger encoded value = nearer.
#define SCENE_DEPTH_SCALE   32000.0f

// Wireframe edges are nudged this fraction nearer (in 1/z space)
// before the depth compare, so an edge reliably beats the coplanar
// face it outlines without z-fighting. Small enough that it does not
// punch an edge through genuinely nearer geometry.
#define SCENE_LINE_BIAS     1.02f

static inline uint16_t scene_depth16(float inv_z) {
    float d = inv_z * SCENE_DEPTH_SCALE;
    if (d < 0.0f)        d = 0.0f;
    if (d > 65535.0f)    d = 65535.0f;
    return (uint16_t)d;
}

// --- Buffers ------------------------------------------------------------------

// Pixel count == framebuffer pixel count; the depth buffer is indexed
// with the exact same direct_565_logical_index() mapping as the fb.
#define SCENE_PIXELS  (DISPLAY_LOG_W * DISPLAY_RAW_STRIDE)

// Deferred wireframe edges. ~80 obstacles * ~14 edges + the ship
// leaves generous headroom; overflow silently drops extra edges.
#define SCENE_LINE_CAP  4096

typedef struct {
    float sx, sy, w;   // projected screen x/y + 1/z depth
} scene_vtx_t;

typedef struct {
    scene_vtx_t v[2];
    uint16_t    packed;
} scene_seg_t;

static uint16_t*   s_depth   = NULL;
static scene_seg_t* s_lines  = NULL;
static int         s_line_n  = 0;

static uint16_t*   s_fb      = NULL;
static bool        s_rev     = false;

void scene_init(void) {
    s_depth = heap_caps_malloc(SCENE_PIXELS * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    s_lines = heap_caps_malloc(SCENE_LINE_CAP * sizeof(scene_seg_t), MALLOC_CAP_SPIRAM);
    if (!s_depth || !s_lines) {
        ESP_LOGE(TAG, "scene buffer allocation failed (depth=%p lines=%p)",
                 s_depth, s_lines);
    }
}

void scene_begin(pax_buf_t* fb) {
    s_fb     = (uint16_t*)pax_buf_get_pixels(fb);
    s_rev    = fb->reverse_endianness;
    s_line_n = 0;
    if (s_depth) {
        memset(s_depth, 0, SCENE_PIXELS * sizeof(uint16_t));
    }
}

// --- Projection ---------------------------------------------------------------

// Project a world point to (screen x, screen y, 1/z). z is clamped to
// the near plane the same way the old per-object renderers clamped
// it, so the projection can't blow up and the visual result matches
// the pre-z-buffer pipeline.
static inline void scene_project(float x, float y, float z, scene_vtx_t* out) {
    if (z < RENDER_NEAR_CLIP_Z) z = RENDER_NEAR_CLIP_Z;
    float const inv_z = 1.0f / z;
    render_camera_t const cam = render_camera();
    out->sx = RENDER_HALF_W    + RENDER_FOCAL_LEN * (x - cam.x) * inv_z;
    out->sy = RENDER_HORIZON_Y - RENDER_FOCAL_LEN * (y - cam.y) * inv_z;
    out->w  = inv_z;
}

// --- Triangle rasterizer ------------------------------------------------------

// Fill one vertical run (logical x fixed) with a per-pixel depth
// test. The depth across the run is the affine function
// w(y) = A*x + B*y + C evaluated incrementally. Under PAX_O_ROT_CW a
// +1 logical-y step is a -1 step in both the fb and depth indices.
static inline void scene_vrun(int lx, int y_top, int y_bot,
                              float A, float B, float C, uint16_t packed) {
    if (lx < 0 || lx >= DISPLAY_LOG_W) return;
    if (y_top < 0)              y_top = 0;
    if (y_bot >= DISPLAY_LOG_H) y_bot = DISPLAY_LOG_H - 1;
    if (y_top > y_bot) return;

    int const idx = direct_565_logical_index(lx, y_top);
    uint16_t* fp  = s_fb    + idx;
    uint16_t* zp  = s_depth + idx;
    float     w   = A * (float)lx + B * (float)y_top + C;
    int       cnt = y_bot - y_top + 1;
    while (cnt-- > 0) {
        uint16_t const d = scene_depth16(w);
        if (d > *zp) { *zp = d; *fp = packed; }
        fp--; zp--;
        w += B;
    }
}

// Depth-tested flat-shaded triangle. Same logical-X column scan as
// direct_565_tri (contiguous raw runs, cache-friendly), plus a
// per-pixel 1/z depth test. The depth plane w = A*x + B*y + C is
// derived from the three vertices' 1/z values before the x-sort.
static void scene_raster_tri(scene_vtx_t a, scene_vtx_t b, scene_vtx_t c,
                             uint16_t packed) {
    // Depth plane through the three (sx, sy, w) points. The cross
    // product of two edge vectors gives the plane normal; nz near
    // zero means a degenerate (zero-area) triangle — skip it.
    float const ex1 = b.sx - a.sx, ey1 = b.sy - a.sy, ew1 = b.w - a.w;
    float const ex2 = c.sx - a.sx, ey2 = c.sy - a.sy, ew2 = c.w - a.w;
    float const nx  = ey1 * ew2 - ew1 * ey2;
    float const ny  = ew1 * ex2 - ex1 * ew2;
    float const nz  = ex1 * ey2 - ey1 * ex2;
    if (nz > -1e-6f && nz < 1e-6f) return;
    float const inv_nz = 1.0f / nz;
    float const A = -nx * inv_nz;
    float const B = -ny * inv_nz;
    float const C = a.w - A * a.sx - B * a.sy;

    // Sort vertices so x0 <= x1 <= x2 (w no longer needed — the plane
    // has been captured in A/B/C).
    float x0 = a.sx, y0 = a.sy, x1 = b.sx, y1 = b.sy, x2 = c.sx, y2 = c.sy;
    float tx, ty;
    if (x1 < x0) { tx=x0; ty=y0; x0=x1; y0=y1; x1=tx; y1=ty; }
    if (x2 < x0) { tx=x0; ty=y0; x0=x2; y0=y2; x2=tx; y2=ty; }
    if (x2 < x1) { tx=x1; ty=y1; x1=x2; y1=y2; x2=tx; y2=ty; }

    if (x2 <= 0.0f || x0 >= (float)DISPLAY_LOG_W) return;
    if (x2 - x0 < 1e-6f) return;

    float const dydx_02 = (y2 - y0) / (x2 - x0);
    float const dydx_01 = (x1 > x0) ? (y1 - y0) / (x1 - x0) : 0.0f;
    float const dydx_12 = (x2 > x1) ? (y2 - y1) / (x2 - x1) : 0.0f;

    int ix_start =     (int)ceilf(x0);
    int ix_split =     (int)ceilf(x1);
    int ix_endex = 1 + (int)floorf(x2);
    if (ix_start < 0)             ix_start = 0;
    if (ix_endex > DISPLAY_LOG_W) ix_endex = DISPLAY_LOG_W;
    if (ix_split < ix_start)      ix_split = ix_start;
    if (ix_split > ix_endex)      ix_split = ix_endex;

    for (int x = ix_start; x < ix_split; x++) {
        float const dx = (float)x - x0;
        float const ya = y0 + dydx_02 * dx;
        float const yb = y0 + dydx_01 * dx;
        float yt, yz;
        if (ya < yb) { yt = ya; yz = yb; } else { yt = yb; yz = ya; }
        scene_vrun(x, (int)ceilf(yt), (int)floorf(yz), A, B, C, packed);
    }
    for (int x = ix_split; x < ix_endex; x++) {
        float const dx02 = (float)x - x0;
        float const dx12 = (float)x - x1;
        float const ya   = y0 + dydx_02 * dx02;
        float const yb   = y1 + dydx_12 * dx12;
        float yt, yz;
        if (ya < yb) { yt = ya; yz = yb; } else { yt = yb; yz = ya; }
        scene_vrun(x, (int)ceilf(yt), (int)floorf(yz), A, B, C, packed);
    }
}

// --- Line rasterizer ----------------------------------------------------------

// Depth-tested wireframe edge. Bresenham line with 1/z interpolated
// along it; tests the depth buffer (with the SCENE_LINE_BIAS nudge)
// but never writes it — an edge is an overlay, not a depth occluder.
static void scene_raster_line(scene_vtx_t a, scene_vtx_t b, uint16_t packed) {
    int const x0 = (int)lroundf(a.sx), y0 = (int)lroundf(a.sy);
    int const x1 = (int)lroundf(b.sx), y1 = (int)lroundf(b.sy);

    int const dx = abs(x1 - x0);
    int const dy = abs(y1 - y0);
    int const sx = (x0 < x1) ? 1 : -1;
    int const sy = (y0 < y1) ? 1 : -1;
    int       err = dx - dy;

    int const steps = (dx > dy) ? dx : dy;
    float     w     = a.w;
    float const dw  = (steps > 0) ? (b.w - a.w) / (float)steps : 0.0f;

    int const ptr_dx = (sx > 0) ? DISPLAY_RAW_STRIDE : -DISPLAY_RAW_STRIDE;
    int const ptr_dy = (sy > 0) ? -1 : 1;

    int const idx = direct_565_logical_index(x0, y0);
    uint16_t* fp  = s_fb    + idx;
    uint16_t* zp  = s_depth + idx;
    int       lx  = x0;
    int       ly  = y0;

    while (1) {
        if (lx >= 0 && lx < DISPLAY_LOG_W && ly >= 0 && ly < DISPLAY_LOG_H) {
            uint16_t const d = scene_depth16(w * SCENE_LINE_BIAS);
            if (d >= *zp) *fp = packed;
        }
        if (lx == x1 && ly == y1) break;
        int const e2 = 2 * err;
        if (e2 > -dy) { err -= dy; lx += sx; fp += ptr_dx; zp += ptr_dx; }
        if (e2 <  dx) { err += dx; ly += sy; fp += ptr_dy; zp += ptr_dy; }
        w += dw;
    }
}

// --- Public submit / flush ----------------------------------------------------

void scene_tri(float x0, float y0, float z0,
               float x1, float y1, float z1,
               float x2, float y2, float z2, uint32_t argb) {
    if (!s_depth) return;
    // Whole-triangle near cull: drop it only if every vertex is
    // behind the near plane (otherwise the per-vertex clamp in
    // scene_project keeps the projection bounded).
    if (z0 < RENDER_NEAR_CLIP_Z && z1 < RENDER_NEAR_CLIP_Z && z2 < RENDER_NEAR_CLIP_Z) {
        return;
    }
    scene_vtx_t a, b, c;
    scene_project(x0, y0, z0, &a);
    scene_project(x1, y1, z1, &b);
    scene_project(x2, y2, z2, &c);
    scene_raster_tri(a, b, c, direct_565_pack(argb, s_rev));
}

void scene_line(float x0, float y0, float z0,
                float x1, float y1, float z1, uint32_t argb) {
    if (!s_lines) return;
    if (z0 < RENDER_NEAR_CLIP_Z && z1 < RENDER_NEAR_CLIP_Z) return;
    if (s_line_n >= SCENE_LINE_CAP) return;
    scene_seg_t* seg = &s_lines[s_line_n++];
    scene_project(x0, y0, z0, &seg->v[0]);
    scene_project(x1, y1, z1, &seg->v[1]);
    seg->packed = direct_565_pack(argb, s_rev);
}

void scene_flush(void) {
    for (int i = 0; i < s_line_n; i++) {
        scene_raster_line(s_lines[i].v[0], s_lines[i].v[1], s_lines[i].packed);
    }
    s_line_n = 0;
}
