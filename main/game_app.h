#pragma once
// =====================================================================
//  Race the Synth  --  application/controller shared state
// ---------------------------------------------------------------------
//  The game's top-level state machine was split out of main.c into the
//  per-state frame modules (screens.c, play_states.c). main.c stays the
//  controller: it owns the shared state below (one definition each) and
//  the run-lifecycle helpers; the frame modules read/write that state
//  through these `extern` declarations. on_render is now just a dispatch
//  switch over app_state.
//
//  This is deliberately a wide, globals-based contract -- the game's
//  controller has always been globals-driven; this header just makes the
//  sharing explicit so the per-state code can live in its own files.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>

#include "game.h"    // game_state_t
#include "world.h"   // world_state_t
#include "save.h"    // save_data_t

// ---- App state machine ----------------------------------------------
typedef enum {
    APP_STATE_SLOT_SELECT = 0,  // first state on boot: pick save slot 0..2
    APP_STATE_MENU,             // main menu: Daily/Seeded/Upgrade/Stats/Settings/Exit
    APP_STATE_SEED_INPUT,       // numeric entry for the custom seed
    APP_STATE_STATS_VIEW,       // text dump of the active slot's stats
    APP_STATE_UPGRADE,          // equip screen: slot list, shows what's fitted
    APP_STATE_UPGRADE_PICK,     // attachment picker for the selected slot
    APP_STATE_CREDITS,          // auto-scrolling credits roll
    APP_STATE_SETTINGS,         // settings submenu: Controls / Display / Audio
    APP_STATE_CONTROLS,         // controls list: gyro checkbox + 4 keybinds
    APP_STATE_DISPLAY,          // brightness sliders: screen / keyboard / LEDs
    APP_STATE_AUDIO_SETTINGS,   // volume slider + three checkboxes: music/SFX/hum
    APP_STATE_PLAYING,
    APP_STATE_PAUSED,           // pause overlay: Resume / Abort run
    APP_STATE_CRASHING,         // post-crash: ship -> spark shower, world still flows
    APP_STATE_STALL_OUT,        // post-stall: frozen scene held a beat before GAME_OVER
    APP_STATE_GAME_OVER,
    APP_STATE_CHECKPOINT_REDO,  // post-crash with a checkpoint: run rewound, "Re-Do" dialog
} app_state_t;

// Pause-menu entries (STATE_PAUSED).
enum {
    PAUSE_ENTRY_RESUME = 0,
    PAUSE_ENTRY_SETTINGS,
    PAUSE_ENTRY_ABORT,
    PAUSE_ENTRY_COUNT,
};

// Menu entry indices for STATE_MENU. Order is the visible order.
enum {
    MENU_ENTRY_DAILY = 0,
    MENU_ENTRY_SEEDED,
    MENU_ENTRY_UPGRADE,
    MENU_ENTRY_STATS,
    MENU_ENTRY_SETTINGS,
    MENU_ENTRY_CREDITS,
    MENU_ENTRY_EXIT,
    MENU_ENTRY_COUNT,
};

// Settings submenu entries (STATE_SETTINGS).
enum {
    SETTINGS_ENTRY_CONTROLS = 0,
    SETTINGS_ENTRY_DISPLAY,
    SETTINGS_ENTRY_AUDIO,
    SETTINGS_ENTRY_COUNT,
};

// Controls-menu entries (STATE_CONTROLS): gyro checkbox first, then the
// four remappable keybinds in CONTROL_KEY_* order so the row index past
// the checkbox maps straight to a controls_key_t.
enum {
    CONTROLS_ENTRY_GYRO = 0,
    CONTROLS_ENTRY_LEFT,
    CONTROLS_ENTRY_RIGHT,
    CONTROLS_ENTRY_ITEM,
    CONTROLS_ENTRY_PAUSE,
    CONTROLS_ENTRY_COUNT,
};

// Display-settings cursor entries (STATE_DISPLAY).
enum {
    DISPLAY_ENTRY_SCREEN = 0,
    DISPLAY_ENTRY_KEYBOARD,
    DISPLAY_ENTRY_LEDS,
    DISPLAY_ENTRY_COUNT,
};

// Audio-settings cursor entries (STATE_AUDIO_SETTINGS).
enum {
    AUDIO_ENTRY_VOLUME = 0,
    AUDIO_ENTRY_MUSIC,
    AUDIO_ENTRY_SFX,
    AUDIO_ENTRY_HUM,
    AUDIO_ENTRY_COUNT,
};

// Number of game-over flavour lines. Keep in sync with the
// gameover_flavours[] definition in main.c (a small, rarely-touched pool;
// a plain count avoids a sizeof() on the extern array from play_states.c).
#define GAMEOVER_FLAVOUR_COUNT 3

// ---- Shared controller state (defined in main.c) --------------------
// Simulation + persistence.
extern game_state_t  game;
extern world_state_t world;
extern save_data_t   s_save;
extern int           s_active_slot;
extern app_state_t   app_state;
extern bool          run_end_committed;
extern uint32_t      daily_seed;

// Per-screen cursors + seed-entry buffer (reset on entry to each screen).
extern int  s_settings_cursor;
extern app_state_t s_settings_origin;  // where the settings family returns on Esc
extern int  s_controls_cursor;
extern int  s_display_cursor;
extern int  s_audio_cursor;
extern int  s_slot_cursor;
extern int  s_menu_cursor;
extern int  s_pause_cursor;
extern int  s_upgrade_cursor;
extern int  s_upgrade_slot;
extern int  s_upgrade_pick_cursor;
extern char s_seed_buf[11];
extern int  s_seed_len;

// Game-over flavour pool + the index chosen on the run-end transition.
extern char const* const gameover_flavours[];
extern int s_gameover_flavour_idx;

// Run bookkeeping read/written by the gameplay states.
extern int    s_peak_stage;
extern double s_run_play_seconds;
extern bool   s_run_was_crash;
extern double s_crash_anim_time;
extern double s_stall_hold_time;
extern bool   s_redo_ignore_pickup;
extern bool   s_godmode;

// Per-frame input snapshot (latched in on_update; the frame functions
// read it directly) + the end-of-run signals the physics pass computes.
extern bool  s_in_pickup;
extern int   s_in_menu_nav;
extern int   s_in_menu_horiz;
extern bool  s_in_menu_esc;
extern bool  s_in_menu_bs;
extern bool  s_in_pause;
extern int   s_in_typed;
extern bool  s_in_typed_d;
extern bool  s_crashed;
extern bool  s_stalled;

// Profiling split point: a frame function records esp_timer_get_time()
// here right after its obstacle/scene render so on_render can attribute
// the obs vs fgrest buckets. Menu states set it at the top of their draw.
extern int64_t t_after_obs;

// ---- Run-lifecycle helpers (defined in main.c) ----------------------
void start_run(game_state_t* game, world_state_t* world, uint32_t seed, bool is_custom);
void save_apply_day_rollover(save_data_t* s);
void end_run_audio(void);
void pause_audio_for_pause_menu(void);
void resume_audio_from_pause_menu(void);
void commit_run_end(game_state_t const* g, world_state_t const* w, bool head_on);
