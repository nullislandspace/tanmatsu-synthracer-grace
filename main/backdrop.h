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
//  The floor grid + obstacle shadows are NOT here -- main.c's on_backdrop
//  draws them (on the CPU) after these submits, in parallel with the
//  mountain BLEND. The submit *order* and the waits between them are the
//  on_backdrop choreography, not this module: PPA does not order ops
//  across clients, so the sun (SRM) and mountains (BLEND) must be
//  serialised against the sky (FILL) by the caller via se_ppa_wait_one().
//
//  Layout constants (cache sizes, y-biases, sky/key colours, the horizon
//  row) live in magicnumbers.h.
// =====================================================================

#include <stdbool.h>

// One-time setup, called from on_init after synthwave_init(). Brings up
// the engine PPA compositor (se_ppa_init), allocates the sun + mountain
// layer caches in PSRAM matching the engine framebuffers' format /
// orientation (queried via se_display_info), renders the synthwave artwork
// into them and flushes them for DMA. Errors are logged (non-fatal); a
// failed init just leaves the per-frame submits as no-ops.
void backdrop_init(void);

// Per-frame composite steps (non-blocking PPA submits onto the game_ui
// `fb` bridge). FILL paints the sky band; SUN copies the sun cache with
// its top at `dest_top_log_y` (logical); BLEND composites the mountain
// cache with a green colour-key. Each returns false (and logs) if the
// submit was refused or failed. Serialise them with se_ppa_wait_one().
bool backdrop_submit_fill_sky(void);
bool backdrop_submit_sun(int dest_top_log_y);
bool backdrop_submit_mountains(void);
