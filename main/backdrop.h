#pragma once
// =====================================================================
//  Race the Synth  --  synthwave backdrop (PPA compositor)
// ---------------------------------------------------------------------
//  The sky / sun / mountain composite that paints the upper (above-
//  horizon) band of the framebuffer each frame. This module owns only
//  the *synthwave content*: the pre-rendered sun + mountain layer caches
//  and the recipe for each composite step (which rows, which colour,
//  which colour-key). The PPA hardware mechanics -- clients, the async
//  completion latch, the logical->raw orientation maths, the aligned
//  PSRAM allocation -- live in the engine (se_ppa.h).
//
//  The floor *base* IS here now (backdrop_submit_fill_floor — a PPA FILL of
//  the below-horizon band, on the same FILL client as the sky). The floor
//  grid lines + obstacle shadows are NOT -- main.c's on_backdrop draws those
//  (on the CPU) on top of the filled base, in parallel with the mountain
//  BLEND. Each submit is tagged with a job id and the engine pump runs them
//  in submission order, so on_backdrop owns only the *choreography*: which id
//  each step gets and where it se_ppa_wait_job()s for a result the CPU needs.
//
//  Layout constants (cache sizes, y-biases, sky/key colours, the horizon
//  row) live in magicnumbers.h.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>

// One-time setup, called from on_init after synthwave_init(). Brings up
// the engine PPA compositor (se_ppa_init), allocates the sun + mountain
// layer caches in PSRAM matching the engine framebuffers' format /
// orientation (queried via se_display_info), renders the synthwave artwork
// into them and flushes them for DMA. Errors are logged (non-fatal); a
// failed init just leaves the per-frame submits as no-ops.
void backdrop_init(void);

// Per-frame composite steps (non-blocking PPA enqueues onto the game_ui
// `fb` bridge, each tagged with the caller's `job_id`). FILL_SKY paints the
// sky band; FILL_FLOOR paints the below-horizon base (`fully_shadowed` picks
// the shadow colour after full sunset); SUN sprite-blits the sun's bounding
// box with its top at `dest_top_log_y` (logical), clipped at the horizon;
// BLEND composites the mountain cache with a green colour-key. Each returns
// false (and logs) if the submit was refused. The engine pump runs them in
// submission order; wait for a step with se_ppa_wait_job(job_id).
bool backdrop_submit_fill_sky(uint32_t job_id);
bool backdrop_submit_fill_floor(uint32_t job_id, bool fully_shadowed);
bool backdrop_submit_sun(uint32_t job_id, int dest_top_log_y);
bool backdrop_submit_mountains(uint32_t job_id);
