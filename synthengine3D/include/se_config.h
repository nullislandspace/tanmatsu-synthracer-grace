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

// ---- Save framework -------------------------------------------------
//
// Number of independent save slots the engine manages (se_save.h). Each
// slot is one file under the configured save directory. Override per
// game (some games want a single slot, others many).
#ifndef SE_SAVE_SLOT_COUNT
#define SE_SAVE_SLOT_COUNT  3
#endif

// ---- Application framework / run loop -------------------------------
//
// se_run()'s per-frame delta-time (seconds) is clamped to at most this
// before being handed to on_update(), so a long stall (debugger pause,
// flash erase, scheduling hiccup) can't teleport the simulation with a
// huge dt. The game sees a capped step instead. Override per game if its
// fixed-step budget differs.
#ifndef SE_FRAME_DT_MAX
#define SE_FRAME_DT_MAX  0.1f
#endif

// ---- Renderer / pinhole-camera projection ---------------------------
//
// Parameters of the software 3D scene's projection (se_scene.h). World
// space is x = lateral, y = up, z = forward (into the screen); the camera
// sits at z = 0. A world point projects as:
//     sx = RENDER_HALF_W    + RENDER_FOCAL_LEN * (x - cam_x) / z
//     sy = RENDER_HORIZON_Y - RENDER_FOCAL_LEN * (y - cam_y) / z
// RENDER_HORIZON_Y is the screen row the horizon vanishes to — a game
// with its own backdrop should override it to match (Race the Synth sets
// it to its synthwave horizon). All overridable per game.
#ifndef RENDER_HALF_W
#define RENDER_HALF_W       ((float)DISPLAY_LOG_W / 2.0f)
#endif
#ifndef RENDER_HORIZON_Y
#define RENDER_HORIZON_Y    256.0f
#endif
#ifndef RENDER_FOCAL_LEN
#define RENDER_FOCAL_LEN    450.0f
#endif
#ifndef RENDER_CAM_Y
#define RENDER_CAM_Y        1.0f    // camera's resting (grounded) height
#endif
// Near-plane z below which the projection blows up; geometry is clamped
// to it and wholly-behind triangles/edges are dropped.
#ifndef RENDER_NEAR_CLIP_Z
#define RENDER_NEAR_CLIP_Z  0.5f
#endif
