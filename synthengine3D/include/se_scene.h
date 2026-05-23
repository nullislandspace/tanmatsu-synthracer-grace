#pragma once
// =====================================================================
//  SynthEngine3D  --  PUBLIC STABLE API  --  3D scene pipeline
// ---------------------------------------------------------------------
//  The software 3D renderer: a per-pixel z-buffered rasterizer plus the
//  pinhole camera + projection it draws through. Games submit world-space
//  triangles / wireframe edges; the engine projects, depth-tests and
//  rasterizes them. The engine knows nothing about game objects — the
//  game iterates its own world and calls scene_tri / scene_line. Part of
//  the semver'd public surface (see se_version.h); projection constants
//  are overridable defaults in se_config.h.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>

#include "pax_gfx.h"
#include "se_config.h"   // RENDER_* projection params (overridable)

// Depth-buffered 3D scene pipeline.
//
// Replaces the old per-object painter's algorithm in render.c. Every
// 3D object (obstacles, the ship) submits its geometry as world-space
// triangles and wireframe edges; the scene module projects it, depth-
// tests it per pixel against a z-buffer, and rasterizes it. Draw order
// no longer matters — the depth test resolves visibility, so objects
// can be submitted in any order and the result is per-pixel correct
// (stacked, straddling and interpenetrating geometry all just work).
//
// Depth is stored as a scaled reciprocal-z: larger value = nearer.
// The depth buffer is never bulk-cleared: a parallel per-pixel frame
// stamp marks which depths belong to the current frame, so a stale
// pixel reads as infinitely far and scene_begin costs one counter
// increment instead of a full-screen memset.
//
// Triangles are rasterized immediately on submit — the z-buffer makes
// their order irrelevant. Wireframe edges are deferred and drawn by
// scene_flush() after every triangle, depth-tested with a small bias
// so an edge wins against the coplanar face it outlines but still
// loses to genuinely nearer geometry.

// Allocate the depth buffer, frame-stamp plane and deferred-edge
// buffer (all PSRAM). Call once at boot, before the first frame.
void scene_init(void);

// Begin a frame's 3D pass: bind the framebuffer, advance the frame
// stamp (logically emptying the depth buffer), reset the deferred-
// edge buffer. Call once per frame before any scene_tri / scene_line.
void scene_begin(pax_buf_t* fb);

// Submit a world-space triangle. Projected and rasterized immediately
// with a per-pixel depth test + write. `argb` is an ARGB8888 colour.
void scene_tri(float x0, float y0, float z0,
               float x1, float y1, float z1,
               float x2, float y2, float z2, uint32_t argb);

// Submit a world-space wireframe edge. Deferred until scene_flush().
void scene_line(float x0, float y0, float z0,
                float x1, float y1, float z1, uint32_t argb);

// Rasterize every deferred edge. Call once after all triangles and
// edges for the frame have been submitted.
void scene_flush(void);

// --- Camera & projection ---------------------------------------------
//
// The scene projects through a single module-global pinhole camera, set
// once per frame before submitting geometry. Kept as a struct so it can
// gain fields later (zoom, shake, look-ahead) without touching call
// sites. `x` is the lateral eye position, `y` the eye height; both are
// world units. The projection itself uses the RENDER_* constants from
// se_config.h (overridable per game).
typedef struct {
    float x;
    float y;
} render_camera_t;

// Set / read the scene camera. Call render_set_camera() once per frame
// before the first scene_tri / scene_line.
void            render_set_camera(float x, float y);
render_camera_t render_camera(void);

// Project a world point (x_w, y_w, z_w) to screen pixels using the
// current camera. y = 0 is the ground plane, +y up, +z forward (away
// from the camera). Out values are in pax logical pixels. (scene_tri /
// scene_line project internally; this is for game code that needs to
// project a point itself — e.g. drawing a floor shadow.)
void render_project(float x_w, float y_w, float z_w, float* out_sx, float* out_sy);
