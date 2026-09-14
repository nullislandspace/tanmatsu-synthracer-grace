#pragma once
// =====================================================================
//  Race the Synth  --  optional external VFD status display
// ---------------------------------------------------------------------
//  Some Tanmatsu units carry a NE-HCS12SS59T-R1 I2C VFD (12-character
//  ASCII) jumpered to address 0x13 (the default 0x10 collides with an
//  on-board device on the internal bus). It is looked for on the CATT
//  port I2C bus first, then on the internal I2C bus -- the same probe as
//  the tanmatsu-vfdclock app.
//
//  All I2C traffic runs in a small background task, so the render loop
//  never waits on the bus: the game only publishes the wanted content
//  with vfd_set_mode() (a lock-free store, safe to call every frame) and
//  the task writes the display when it changes. Without a VFD the task
//  exits after the probe and every call here is a cheap no-op.
//
//    VFD_MODE_IDLE    no race running: hardware-scrolls
//                     "RACE THE SYNTH BY CAVAC"
//    VFD_MODE_STAGE   racing: "STAGE N"
//    VFD_MODE_PAUSED  run paused: blinks "PAUSED"
// =====================================================================

typedef enum {
    VFD_MODE_IDLE = 0,
    VFD_MODE_STAGE,
    VFD_MODE_PAUSED,
} vfd_mode_t;

// Start the VFD task: probe both buses, and if the display is found
// enable it, set a safe filament current and show VFD_MODE_IDLE. Call
// once from on_init. Non-blocking.
void vfd_init(void);

// Publish what the display should show. `stage` is used only by
// VFD_MODE_STAGE. Cheap; call every frame.
void vfd_set_mode(vfd_mode_t mode, int stage);

// Blank and switch off the display, then stop the task. Blocks until the
// task is done (bounded), so call it right before returning to the
// launcher. A no-op when no VFD was found.
void vfd_shutdown(void);
