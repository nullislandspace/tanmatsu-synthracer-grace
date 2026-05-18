#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pax_gfx.h"

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
// The buffer is cleared to 0 (infinitely far) at scene_begin.
//
// Triangles are rasterized immediately on submit — the z-buffer makes
// their order irrelevant. Wireframe edges are deferred and drawn by
// scene_flush() after every triangle, depth-tested with a small bias
// so an edge wins against the coplanar face it outlines but still
// loses to genuinely nearer geometry.

// Allocate the depth buffer and the deferred-edge buffer (both PSRAM).
// Call once at boot, before the first frame.
void scene_init(void);

// Begin a frame's 3D pass: bind the framebuffer, clear the depth
// buffer, reset the deferred-edge buffer. Call once per frame before
// any scene_tri / scene_line.
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
