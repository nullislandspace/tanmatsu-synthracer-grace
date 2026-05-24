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
// render_run_scene rasterises the depth-buffered 3D scene (obstacles +
// optionally the ship); the two overlays are the GAME_OVER and Re-Do
// dialogs drawn over a frozen run.
void render_run_scene(world_state_t const* w, game_state_t const* g, bool draw_ship);
void draw_game_over_overlay(void);
void draw_checkpoint_redo_overlay(void);
