// Per-app control settings persisted in the `synthracer` NVS namespace
// (the same namespace audio_settings uses). These are device-global,
// not per-save-slot: a player's preferred keys and gyro preference
// should survive switching save slots.
//
// Stored keys:
//   ctl_gyro   (u8)  — gyroscope steering enable. Default 0 (off).
//                      Not wired to gameplay yet; stored for a future
//                      gyro-input implementation.
//   ctl_k_left / ctl_k_right / ctl_k_item / ctl_k_pause (u16) —
//                      the BSP scancode bound to each in-run action.
//                      Defaults: ESC / BACKSPACE / SPACE / F4.
//
// Keybinds are stored as raw `bsp_input_scancode_t` values. Every
// physical key on the Tanmatsu keyboard has a scancode (see
// bsp/input.h), so a scancode is a complete, channel-independent
// identifier — it can be polled via bsp_input_read_scancode() for
// smooth steering and matched against scancode events for edges.

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

// Load all control settings from NVS. Missing keys fall back to the
// documented defaults. Safe to call once at boot.
esp_err_t controls_settings_load(void);

bool controls_settings_gyro_on(void);
void controls_settings_set_gyro_on(bool on);

// Current scancode bound to `which`. Returns the default if unset.
uint16_t controls_settings_key(controls_key_t which);

// Rebind `which` to `scancode` and persist it. No-op if unchanged.
void controls_settings_set_key(controls_key_t which, uint16_t scancode);
