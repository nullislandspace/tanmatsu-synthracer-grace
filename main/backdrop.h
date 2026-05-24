#pragma once
// =====================================================================
//  Race the Synth  --  synthwave backdrop (PPA compositor)
// ---------------------------------------------------------------------
//  The PPA-driven sky / sun / mountain composite that paints the upper
//  (above-horizon) band of the framebuffer each frame. Owns the three
//  PPA clients (FILL / SRM / BLEND), the pre-rendered sun + mountain
//  layer caches in PSRAM, and the completion semaphore. The floor grid
//  + obstacle shadows are NOT here -- they are drawn by main.c's
//  on_backdrop after these submits, in parallel with the BLEND.
//
//  Layout constants (cache sizes, y-biases, sky/key colours, the
//  horizon row) live in magicnumbers.h.
// =====================================================================

#include <stdbool.h>
#include <stddef.h>

#include "pax_gfx.h"   // pax_buf_type_t, pax_orientation_t

// One-time setup, called from on_init after synthwave_init(). Allocates
// + renders the sun and mountain layer caches in PSRAM and registers the
// three PPA clients. Pass the engine-resolved framebuffer geometry so
// the caches match its format / endianness / orientation and the band
// math matches its raw dimensions. Errors are logged (non-fatal); a
// failed init just leaves the backdrop submits as no-ops.
void backdrop_init(size_t h_res, size_t v_res,
                   pax_buf_type_t format, bool reversed,
                   pax_orientation_t orientation);

// Per-frame PPA submits (non-blocking) onto the game_ui `fb` bridge.
// FILL paints the sky band; SUN copies the sun cache with its top at
// `dest_top_log_y` (logical); BLEND composites the mountain cache with a
// green colour-key. Each returns false (and logs) on a submit error.
bool backdrop_submit_fill_sky(void);
bool backdrop_submit_sun(int dest_top_log_y);
bool backdrop_submit_mountains(void);

// Block until one pending PPA op completes. Used to serialise the three
// submits (the driver does not guarantee FIFO across client boundaries).
void backdrop_wait_one(void);
