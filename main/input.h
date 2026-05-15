#pragma once

#include <stdbool.h>
#include <stdint.h>

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

// Drain all queued events. Returns true if the user pressed F1
// (caller should restart_to_launcher). Other queued events update
// internal latches that will be observable via the accessors below.
bool input_drain_events(void);

// Returns the steering input as a signed value in [-1.0, +1.0]:
// the D-pad and the remappable Left/Right keys give full-deflection
// ±1 (modal pair gated by input_mode), and — when the Controls
// "Gyroscope" toggle is on and no key is held — the accelerometer
// gives proportional tilt steering. A held key overrides tilt.
float input_steering(void);

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

// Begin key-capture for the Controls remap dialog. While capture is
// active, the next plain key press is latched and every other event
// (steering, menu nav, F1 exit, volume, …) is swallowed so the
// pressed key is bound rather than acted on. Capture ends when
// input_consume_captured_key() returns true.
void input_begin_key_capture(void);

// If a key was captured since input_begin_key_capture(), returns true,
// writes the BSP scancode into *out_scancode, and ends capture mode.
// Returns false while still waiting for a key press.
bool input_consume_captured_key(uint16_t* out_scancode);
