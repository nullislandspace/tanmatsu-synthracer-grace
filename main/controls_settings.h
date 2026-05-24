// Per-app control settings persisted in the `synthracer` NVS namespace
// (the same namespace audio_settings uses). These are device-global,
// not per-save-slot: a player's gyro preference should survive switching
// save slots.
//
// Stored key:
//   ctl_gyro   (u8)  — gyroscope steering enable. Default 0 (off).
//
// The remappable keybinds (Left / Right / Use item / Pause) are no longer
// owned here — they live in the engine's binding subsystem (se_bindings),
// declared by this module's loader and queried via se_bindings_get(). The
// controls_key_t enum below is the set of control ids passed to it.

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// Remappable in-run actions. Order is the visible order in the
// Controls menu (after the gyro checkbox).
typedef enum {
    CONTROL_KEY_LEFT = 0,   // steer left   — default ESC
    CONTROL_KEY_RIGHT,      // steer right  — default BACKSPACE
    CONTROL_KEY_ITEM,       // use item     — default SPACE (not yet wired)
    CONTROL_KEY_PAUSE,      // pause toggle — default F4
    CONTROL_KEY_COUNT,
} controls_key_t;

// Load control settings: reads the gyro flag from NVS (default off) and
// registers the remappable controls with the engine's se_bindings
// subsystem (which loads their persisted scancodes, else the defaults).
// Safe to call once at boot.
esp_err_t controls_settings_load(void);

bool controls_settings_gyro_on(void);
void controls_settings_set_gyro_on(bool on);
