#pragma once

// Display / framebuffer geometry — single source of truth.
//
// The Tanmatsu LCD is physically mounted rotated 270° from the
// user's viewpoint. `bsp_display_get_default_rotation()` returns
// BSP_DISPLAY_ROTATION_270, which we map to `PAX_O_ROT_CW`.
//
// _RAW_  values describe the LCD's native (unrotated) layout —
//        this is how the framebuffer is actually stored in PSRAM.
// _LOG_  values describe what code sees through PAX's coordinate
//        system, i.e. after the orientation transform.
//
// The relationship under PAX_O_ROT_CW is:
//     DISPLAY_LOG_W  ==  DISPLAY_RAW_H
//     DISPLAY_LOG_H  ==  DISPLAY_RAW_W
//
// Centralising these here lets the custom direct-565 line/pixel
// helpers (`direct_565.h`) hardcode rotation + stride into the
// inner loop while keeping the numeric values configurable — port
// to a different display by updating these defines.
#define DISPLAY_RAW_W       480
#define DISPLAY_RAW_H       800

#define DISPLAY_LOG_W       800   // == DISPLAY_RAW_H
#define DISPLAY_LOG_H       480   // == DISPLAY_RAW_W

// Raw-buffer stride, in pixels (not bytes). Equals DISPLAY_RAW_W
// because the framebuffer is a tightly-packed 2D RGB565 array.
#define DISPLAY_RAW_STRIDE  DISPLAY_RAW_W
