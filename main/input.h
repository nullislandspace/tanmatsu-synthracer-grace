#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "bsp/input.h"   // bsp_input_event_t (forwarded by the engine)

// Game state codes that gate the modal steering keys (ESC and Backspace
// are steering only during STATE_PLAYING; everywhere else they have
// their conventional cancel/edit roles). Defined here so input.c can
// resolve the modal binding without depending on game.h.
typedef enum {
    INPUT_MODE_TITLE = 0,
    INPUT_MODE_PLAYING,
    INPUT_MODE_PAUSED,
    INPUT_MODE_GAME_OVER,
    INPUT_MODE_MENU_SEED,
} input_mode_t;

void input_init(void);

// Set the current modal mode. Affects which keys count as steering.
void input_set_mode(input_mode_t mode);

// Process one input event forwarded by the engine's input pump
// (se_run's on_input callback). Updates the internal latches that the
// consume_* accessors below read. The engine consumes the device-global
// keys itself (volume +/-, audio-jack, F1-exit), so those never arrive
// here; everything else (pickup, menu nav, ESC/Backspace, digits, the
// pause + debug keys, and rebind-capture key presses) does.
void input_handle_event(bsp_input_event_t const* ev);

// Returns the steering input as a signed value in [-1.0, +1.0]:
// the D-pad and the remappable Left/Right keys give full-deflection
// ±1 (modal pair gated by input_mode), and — when the Controls
// "Gyroscope" toggle is on and no key is held — the accelerometer
// gives proportional tilt steering. A held key overrides tilt.
float input_steering(void);

// Raw digital steer-button states (LEFT/RIGHT D-pad plus, during
// PLAYING, the configurable steering scancodes). Unlike input_steering()
// these are not collapsed — both can be true at once — so callers can
// detect the both-held barrel-roll gesture. Gyro tilt is not included.
void input_steer_held(bool* out_left, bool* out_right);

// True for the frame where the "use pickup" button (Space or Gamepad-A)
// was just pressed. Self-clears after one read.
bool input_consume_pickup(void);

// Returns the net speed-adjust delta accumulated since the last call:
// +N for N UP-edge presses, -N for DOWN-edges, 0 if neither fired.
// Self-clears after one read. Debug aid for tuning ship_speed_z.
int  input_consume_speed_delta(void);

// Returns the net sun-position delta accumulated since the last call:
// +N for N Q-edge presses (push sun toward sunset),
// -N for N A-edge presses (push sun back toward zenith).
// Self-clears. Debug aid for tuning GAME_SUN_SINK_RANGE_PX and
// previewing shadow lengths without waiting for the natural
// sunset to advance.
int  input_consume_sun_delta(void);

// Menu navigation edge: +1 for an UP press, -1 for a DOWN press, 0 if
// neither pressed since last call. Self-clears. UP/DOWN also feed the
// speed_delta debug knob — the same physical key serves both roles;
// the main loop reads the one appropriate to the current app state.
int  input_consume_menu_nav(void);

// Horizontal menu edge: +1 for a RIGHT press, -1 for a LEFT press, 0 if
// neither pressed since last call. Self-clears. Used by the engine
// menus' RANGE sliders (brightness / volume). LEFT/RIGHT also steer
// during PLAYING via the polled path; this latch is only consumed in
// menu states, so the two roles never collide.
int  input_consume_menu_horiz(void);

// True if ENTER, SPACE or GAMEPAD_A was pressed since last call.
// Mirrors `input_consume_pickup` — they share the same edge buffer
// because the action button doubles as the menu confirm button.
bool input_consume_menu_confirm(void);

// True if ESC was pressed since last call. Only fires in non-PLAYING
// modes; during PLAYING the same key is steering (polled).
bool input_consume_menu_cancel(void);

// True if BACKSPACE was pressed since last call. Same modal rule as
// ESC — only fires in non-PLAYING modes.
bool input_consume_backspace(void);

// If the user typed an ASCII digit since last call, returns true and
// writes the digit into *out_digit (0..9). Self-clears.
bool input_consume_digit(int* out_digit);

// True if F4 was pressed since last call. F4 toggles the in-game
// pause menu: PLAYING → PAUSED (overlay with Resume / Abort) and
// PAUSED → PLAYING (Resume). Self-clears.
bool input_consume_pause_toggle(void);

// True if TAB was pressed since last call. Debug-only hook for
// forcing the world generator to a specific next area type.
// Self-clears.
bool input_consume_force_next_area(void);

// True if G was pressed since last call. Debug-only hook for
// toggling godmode (crash / stall end-of-run disabled). Self-clears.
bool input_consume_godmode_toggle(void);

// (Key-rebind capture moved into the engine: the Controls menu calls the
// engine's blocking se_ui_capture_key(), which drains the input queue
// itself, so input.c no longer exposes a capture mode.)
