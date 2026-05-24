#pragma once
// =====================================================================
//  Race the Synth  --  gameplay states
// ---------------------------------------------------------------------
//  One frame function per gameplay app_state: each renders the run scene
//  + HUD, consumes this frame's input (the s_in_* snapshot + the
//  end-of-run signals s_crashed/s_stalled in game_app.h) and applies its
//  state transitions. on_render dispatches to these by app_state.
// =====================================================================

void play_playing_frame(void);
void play_crashing_frame(void);
void play_stall_out_frame(void);
void play_paused_frame(void);
void play_game_over_frame(void);
void play_checkpoint_redo_frame(void);
