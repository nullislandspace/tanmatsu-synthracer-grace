#pragma once
// =====================================================================
//  Race the Synth  --  in-game HUD readouts + indicators
// ---------------------------------------------------------------------
//  The heads-up overlays drawn during PLAYING / GAME_OVER / paused etc.:
//  score + multiplier panel, stage readout + rest-area stage banner, the
//  debug v=/sun=/god= readouts, and the boost / jump / shield /
//  checkpoint inventory symbols. All draw onto the game_ui `fb` bridge
//  and read live game / world state through const pointers.
// =====================================================================

#include <stdbool.h>

#include "game.h"    // game_state_t
#include "world.h"   // world_state_t

// Top-of-screen readouts.
void draw_score_readout(game_state_t const* g);       // top-right score
void draw_multiplier_panel(game_state_t const* g);    // top-left x-mult panel
void draw_stage_readout(world_state_t const* w);      // top-right "Stage: N"

// Debug readouts (top-right stack, below score). draw_debug_readout shows
// the godmode flag + ship world position; godmode is passed in (it is
// main.c state toggled by the G key). All three compile to no-ops in a
// release build (ENABLE_DEBUGKEYS == 0) -- see hud.c.
void draw_speed_readout(float speed_z);
void draw_sun_readout(float sun_y);
void draw_debug_readout(game_state_t const* g, bool godmode);

// Bottom-corner indicators.
void draw_boost_indicator(game_state_t const* g);       // active-boost triangle
void draw_jump_inventory(game_state_t const* g);        // jump-charge diamonds
void draw_shield_inventory(game_state_t const* g);      // shield-charge hexagons
void draw_checkpoint_inventory(game_state_t const* g);  // held-checkpoint tile

// The "F4 to pause" hint, drawn below the multiplier panel during PLAYING.
void draw_pause_hint(void);

// "Stage: N" rest-area banner + the predicate that gates it (true only in
// the tail of a rest area, just before the next stage begins).
void draw_stage_banner(int stage);
bool stage_banner_visible(world_state_t const* w);
