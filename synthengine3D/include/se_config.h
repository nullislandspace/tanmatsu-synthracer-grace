#pragma once
// =====================================================================
//  SynthEngine3D  --  PUBLIC STABLE API  --  compile-time configuration
// ---------------------------------------------------------------------
//  Engine defaults that must be known at compile time (they fold into
//  hot inner loops). Every value is `#ifndef`-guarded, so the game can
//  override any of them by either:
//    * defining the macro before this header is reached (e.g. in a game
//      override header pulled in first), or
//    * passing -DSE_xxx=... via target_compile_definitions.
//  Values that need no compile-time folding are configured at runtime
//  through the relevant subsystem's init struct instead -- see docs/.
//
//  This header is part of the stable, semver'd public surface
//  (see se_version.h).
// =====================================================================

// ---- Display / framebuffer geometry ---------------------------------
//
// The Tanmatsu LCD is physically mounted rotated 270 degrees from the
// user's viewpoint. `bsp_display_get_default_rotation()` returns
// BSP_DISPLAY_ROTATION_270, which the app maps to `PAX_O_ROT_CW`.
//
// _RAW_  values describe the LCD's native (unrotated) layout -- this is
//        how the framebuffer is actually stored in PSRAM.
// _LOG_  values describe what code sees through PAX's coordinate system,
//        i.e. after the orientation transform.
//
// The relationship under PAX_O_ROT_CW is:
//     DISPLAY_LOG_W  ==  DISPLAY_RAW_H
//     DISPLAY_LOG_H  ==  DISPLAY_RAW_W
//
// Centralising these lets the direct-565 line/pixel helpers
// (se_direct565.h) hardcode rotation + stride into the inner loop while
// keeping the numeric values configurable -- port to a different display
// by overriding these defines.

#ifndef DISPLAY_RAW_W
#define DISPLAY_RAW_W       480
#endif
#ifndef DISPLAY_RAW_H
#define DISPLAY_RAW_H       800
#endif

#ifndef DISPLAY_LOG_W
#define DISPLAY_LOG_W       800   // == DISPLAY_RAW_H
#endif
#ifndef DISPLAY_LOG_H
#define DISPLAY_LOG_H       480   // == DISPLAY_RAW_W
#endif

// Raw-buffer stride, in pixels (not bytes). Equals DISPLAY_RAW_W because
// the framebuffer is a tightly-packed 2D RGB565 array.
#ifndef DISPLAY_RAW_STRIDE
#define DISPLAY_RAW_STRIDE  DISPLAY_RAW_W
#endif

// ---- Audio gain staging ---------------------------------------------
//
// Master gains applied by the mixer (se_audio.h / audio_mixer.c) in Q15
// at mix-down. Per-voice `*_AMP` constants in each source set relative
// loudness; these set the overall music-vs-SFX balance. With 5
// concurrent random-phase SFX voices, summing scales peak by ~sqrt(5)
// (~2.24x); these defaults leave headroom for music + SFX + a drone bed
// without hard-clipping the int16 accumulator. Tune by ear, then check
// the headroom math on paper. Override per game as needed.
#ifndef AUDIO_MUSIC_GAIN
#define AUDIO_MUSIC_GAIN  0.30f
#endif
#ifndef AUDIO_SFX_GAIN
#define AUDIO_SFX_GAIN    0.35f
#endif

// Number of independent SFX mute groups the mixer tracks. Voices carry
// a `group` index in [0, this); the host gates each group via
// audio_mixer_set_group_enabled(). The engine assigns no meaning to any
// group -- that is the host app's choice. Raise if a game needs more
// independent SFX mute categories.
#ifndef SE_AUDIO_SFX_GROUP_COUNT
#define SE_AUDIO_SFX_GROUP_COUNT  4
#endif
