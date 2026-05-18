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
// Depth is the reciprocal of world-z (1/z), the quantity that
// interpolates linearly in screen space under the pinhole
// projection. Near-clipped z is RENDER_NEAR_CLIP_Z (0.5), so 1/z
// peaks at 2.0; SCENE_DEPTH_SCALE maps that to 64000 — inside the
// uint16 range with headroom, so the rasterizer never has to clamp
// the high end. Larger encoded value = nearer.
#define SCENE_DEPTH_SCALE   32000.0f

// Wireframe edges are nudged this fraction nearer (in 1/z space)
// before the depth compare, so an edge reliably beats the coplanar
// face it outlines without z-fighting, while still losing to
// genuinely nearer geometry.
#define SCENE_LINE_BIAS     1.02f

// --- Buffers ------------------------------------------------------------------
//
// The depth buffer is never cleared. Instead a parallel 8-bit stamp
// plane records, per pixel, the frame number that last wrote a depth
// there. A depth value counts only when its stamp equals the current
// frame; a stale stamp reads as "infinitely far". So every frame
// starts with a logically-empty depth buffer for the cost of one
// counter increment — no 768 KB memset — and depth traffic happens
// only on pixels the 3D scene actually draws, not the whole screen.
//
// The stamp is 8-bit, so it wraps every 256 frames; a pixel that an
// obstacle covered, then went exactly a 256-frame multiple without
// being touched, then was covered again, could mis-resolve for one
// pixel for one frame. That is invisible in practice. Frame 0 is
// skipped on wrap so an untouched (zero-initialised) stamp cell never
// matches a live frame.

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

static uint16_t*    s_depth   = NULL;   // encoded 1/z, indexed like the fb
static uint8_t*     s_stamp   = NULL;   // frame tag per pixel
static scene_seg_t* s_lines   = NULL;
static int          s_line_n  = 0;

static uint16_t*    s_fb      = NULL;
static bool         s_rev     = false;
static uint8_t      s_frame   = 0;      // current frame tag (never 0 while live)

void scene_init(void) {
    s_depth = heap_caps_malloc(SCENE_PIXELS * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    s_stamp = heap_caps_malloc(SCENE_PIXELS * sizeof(uint8_t),  MALLOC_CAP_SPIRAM);
    s_lines = heap_caps_malloc(SCENE_LINE_CAP * sizeof(scene_seg_t), MALLOC_CAP_SPIRAM);
    if (!s_depth || !s_stamp || !s_lines) {
        ESP_LOGE(TAG, "scene buffer allocation failed (depth=%p stamp=%p lines=%p)",
                 s_depth, s_stamp, s_lines);
        return;
    }
    // One-time stamp clear so no garbage cell matches the first
    // live frame tag (1). The depth buffer needs no init — a cell is
    // only ever read after its stamp says it was written this frame.
    memset(s_stamp, 0, SCENE_PIXELS * sizeof(uint8_t));
}

void scene_begin(pax_buf_t* fb) {
    s_fb     = (uint16_t*)pax_buf_get_pixels(fb);
    s_rev    = fb->reverse_endianness;
    s_line_n = 0;
    // Advance the frame tag; skip 0 so a zero-initialised stamp cell
    // is never mistaken for "written this frame".
    s_frame++;
    if (s_frame == 0) s_frame = 1;
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
// test. Encoded depth across the run is the affine function
// d(y) = As*x + Bs*y + Cs — already scaled into uint16 units, so the
// inner loop is one float add per pixel (no multiply, no clamp on the
// high end). Under PAX_O_ROT_CW a +1 logical-y step is a -1 step in
// the fb / depth / stamp indices alike.
static inline void scene_vrun(int lx, int y_top, int y_bot,
                              float As, float Bs, float Cs, uint16_t packed) {
    if (lx < 0 || lx >= DISPLAY_LOG_W) return;
    if (y_top < 0)              y_top = 0;
    if (y_bot >= DISPLAY_LOG_H) y_bot = DISPLAY_LOG_H - 1;
    if (y_top > y_bot) return;

    uint8_t const frame = s_frame;
    int const idx = direct_565_logical_index(lx, y_top);
    uint16_t* fp  = s_fb    + idx;
    uint16_t* zp  = s_depth + idx;
    uint8_t*  sp  = s_stamp + idx;
    float     d   = As * (float)lx + Bs * (float)y_top + Cs;
    int       cnt = y_bot - y_top + 1;
    while (cnt-- > 0) {
        int di = (int)d;
        if (di < 0) di = 0;                      // sub-pixel edge overshoot guard
        uint16_t const stored = (*sp == frame) ? *zp : 0;
        if ((uint16_t)di > stored) {
            *zp = (uint16_t)di;
            *sp = frame;
            *fp = packed;
        }
        fp--; zp--; sp--;
        d += Bs;
    }
}

// Depth-tested flat-shaded triangle. Same logical-X column scan as
// direct_565_tri (contiguous raw runs, cache-friendly). The depth
// plane d = As*x + Bs*y + Cs (in encoded uint16 units) is derived
// from the three vertices' 1/z values before the x-sort.
static void scene_raster_tri(scene_vtx_t a, scene_vtx_t b, scene_vtx_t c,
                             uint16_t packed) {
    // Plane through the three (sx, sy, w) points; nz near zero is a
    // degenerate (zero-area) triangle — skip it.
    float const ex1 = b.sx - a.sx, ey1 = b.sy - a.sy, ew1 = b.w - a.w;
    float const ex2 = c.sx - a.sx, ey2 = c.sy - a.sy, ew2 = c.w - a.w;
    float const nx  = ey1 * ew2 - ew1 * ey2;
    float const ny  = ew1 * ex2 - ex1 * ew2;
    float const nz  = ex1 * ey2 - ey1 * ex2;
    if (nz > -1e-6f && nz < 1e-6f) return;
    float const inv_nz = 1.0f / nz;
    // Plane coefficients, pre-scaled into encoded-depth units so the
    // per-pixel run does no multiply.
    float const As = (-nx * inv_nz) * SCENE_DEPTH_SCALE;
    float const Bs = (-ny * inv_nz) * SCENE_DEPTH_SCALE;
    float const Cs = a.w * SCENE_DEPTH_SCALE - As * a.sx - Bs * a.sy;

    // Sort vertices so x0 <= x1 <= x2 (w is now captured in As/Bs/Cs).
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
        scene_vrun(x, (int)ceilf(yt), (int)floorf(yz), As, Bs, Cs, packed);
    }
    for (int x = ix_split; x < ix_endex; x++) {
        float const dx02 = (float)x - x0;
        float const dx12 = (float)x - x1;
        float const ya   = y0 + dydx_02 * dx02;
        float const yb   = y1 + dydx_12 * dx12;
        float yt, yz;
        if (ya < yb) { yt = ya; yz = yb; } else { yt = yb; yz = ya; }
        scene_vrun(x, (int)ceilf(yt), (int)floorf(yz), As, Bs, Cs, packed);
    }
}

// --- Line rasterizer ----------------------------------------------------------

// Depth-tested wireframe edge. Bresenham line with encoded depth
// interpolated along it; tests the depth buffer (with the
// SCENE_LINE_BIAS nudge baked into the endpoint depths) but never
// writes it — an edge is an overlay, not a depth occluder.
static void scene_raster_line(scene_vtx_t a, scene_vtx_t b, uint16_t packed) {
    int const x0 = (int)lroundf(a.sx), y0 = (int)lroundf(a.sy);
    int const x1 = (int)lroundf(b.sx), y1 = (int)lroundf(b.sy);

    int const dx = abs(x1 - x0);
    int const dy = abs(y1 - y0);
    int const sx = (x0 < x1) ? 1 : -1;
    int const sy = (y0 < y1) ? 1 : -1;
    int       err = dx - dy;

    int   const steps = (dx > dy) ? dx : dy;
    float const eda   = a.w * (SCENE_LINE_BIAS * SCENE_DEPTH_SCALE);
    float const edb   = b.w * (SCENE_LINE_BIAS * SCENE_DEPTH_SCALE);
    float       d     = eda;
    float const dd    = (steps > 0) ? (edb - eda) / (float)steps : 0.0f;

    uint8_t const frame  = s_frame;
    int     const ptr_dx = (sx > 0) ? DISPLAY_RAW_STRIDE : -DISPLAY_RAW_STRIDE;
    int     const ptr_dy = (sy > 0) ? -1 : 1;

    int const idx = direct_565_logical_index(x0, y0);
    uint16_t* fp  = s_fb    + idx;
    uint16_t* zp  = s_depth + idx;
    uint8_t*  sp  = s_stamp + idx;
    int       lx  = x0;
    int       ly  = y0;

    while (1) {
        if (lx >= 0 && lx < DISPLAY_LOG_W && ly >= 0 && ly < DISPLAY_LOG_H) {
            int di = (int)d;
            if (di < 0) di = 0;
            uint16_t const stored = (*sp == frame) ? *zp : 0;
            if ((uint16_t)di >= stored) *fp = packed;
        }
        if (lx == x1 && ly == y1) break;
        int const e2 = 2 * err;
        if (e2 > -dy) { err -= dy; lx += sx; fp += ptr_dx; zp += ptr_dx; sp += ptr_dx; }
        if (e2 <  dx) { err += dx; ly += sy; fp += ptr_dy; zp += ptr_dy; sp += ptr_dy; }
        d += dd;
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
