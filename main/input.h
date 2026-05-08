#pragma once

#include <stdbool.h>

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

// Returns the steering input as a signed value in {-1, 0, +1} after
// OR-ing all three left/right paths (D-pad / A&D / Esc&Backspace), with
// the modal pair gated by the current input_mode.
int  input_steering(void);

// True for the frame where the "use pickup" button (Space or Gamepad-A)
// was just pressed. Self-clears after one read.
bool input_consume_pickup(void);

// Returns the net speed-adjust delta accumulated since the last call:
// +N for N UP-edge presses, -N for DOWN-edges, 0 if neither fired.
// Self-clears after one read. Debug aid for tuning ship_speed_z.
int  input_consume_speed_delta(void);
