#pragma once
// =====================================================================
//  Race the Synth  --  non-gameplay screen states
// ---------------------------------------------------------------------
//  One frame function per non-gameplay app_state: each draws its screen,
//  consumes this frame's input (from the s_in_* snapshot in game_app.h)
//  and applies its state transitions. on_render dispatches to these by
//  app_state. The shared scene/overlay draw helpers are declared here too
//  because the gameplay states (play_states.c) reuse them.
// =====================================================================

#include "game.h"    // game_state_t
#include "world.h"   // world_state_t

// Per-state frame functions (draw + input + transitions).
void screen_slot_select_frame(void);
void screen_menu_frame(void);
void screen_seed_input_frame(void);
void screen_settings_frame(void);
void screen_controls_frame(void);
void screen_display_frame(void);
void screen_audio_frame(void);
void screen_stats_frame(void);
void screen_upgrade_frame(void);
void screen_upgrade_pick_frame(void);
void screen_credits_frame(void);

// Shared draw helpers reused by the gameplay states (play_states.c).
// The 3D scene is drawn in two phases so the geometry-only prepare can
// overlap the PPA backdrop DMA: render_prepare_scene() (emit + cull + order,
// no framebuffer access) runs in on_backdrop; render_rasterize_scene()
// paints it in on_render after the backdrop is down. render_run_scene() is
// the one-shot prepare+rasterize, kept for non-overlapping callers. The two
// overlays are the GAME_OVER and Re-Do dialogs drawn over a frozen run.
void render_prepare_scene(world_state_t const* w, game_state_t const* g, bool draw_ship);
void render_rasterize_scene(void);
void render_run_scene(world_state_t const* w, game_state_t const* g, bool draw_ship);
void draw_game_over_overlay(void);
void draw_checkpoint_redo_overlay(void);
