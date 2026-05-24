# Changelog

All notable changes to SynthEngine3D's **public API** (everything under
`include/`). The format follows [Keep a Changelog](https://keepachangelog.com/),
and the project follows semantic versioning as defined in `se_version.h`:

- **MAJOR** — incompatible public-API change (signatures, semantics, removed
  symbols, changed public struct layout).
- **MINOR** — backwards-compatible additions to the public API.
- **PATCH** — internal-only changes (optimisation, refactor, bugfix) with no
  public-API effect.

While the version is `0.x`, the surface is documented and semver-tracked but
not yet frozen — minor releases may still adjust the API as it settles toward
`1.0`.

## [0.1.0] — 2026-05-24

First documented release: the engine is feature-complete for its source game
and has a full doc suite. Extracted from **Race the Synth** across phases
E0–EF + ER (see `../devdocs/engine-extraction.md` for the extraction history).

### Added — public API surface
- **`se_run.h`** — application-framework run loop (`se_run`, `se_app_config_t`,
  `se_app_callbacks_t`, `se_request_exit`, `se_display_info`).
- **`se_scene.h`** — z-buffered 3D renderer + pinhole camera + projection
  (`scene_init/begin/tri/line/render/flush`, `se_render_mode_t`,
  `render_set_camera`/`render_camera`/`render_project`). Deferred pipeline
  (ER): `scene_tri`/`scene_line` accumulate; `scene_render(mode)` rasterizes.
- **`se_audio.h`** + **`se_audio_source.h`** — software mixer + the
  `music_source_t` / `sfx_voice_t` source contracts, app-pushed mute groups.
- **`se_audio_dsp.h`** — oscillator / envelope / biquad DSP primitives.
- **`se_music_procedural.h`** — seed-driven procedural music source.
- **`se_ui.h`** — data-driven list menus (`se_menu_def_t` / `se_menu_t`,
  `se_menu_input` / `se_menu_draw`, row kinds NONE/CHECK/TEXT/CUSTOM/RANGE)
  + the blocking `se_ui_capture_key` rebind modal.
- **`se_bindings.h`** — remappable, NVS-persisted key bindings.
- **`se_hw.h`** — device-global hardware settings (volume + 3 brightnesses):
  boot-apply, in-game get/set, persistence to the launcher-shared NVS.
- **`se_save.h`** + **`se_nbt.h`** — file-backed save slots with a peek header,
  over an NBT serialization primitive.
- **`se_text.h`** — Hershey vector text.
- **`se_direct565.h`** — inline RGB565 framebuffer primitives.
- **`se_config.h`** — overridable compile-time defaults (display, projection,
  audio gains, UI theme, save-slot count, bindings cap).
- **`synthengine3d.h`** — umbrella header.

### Known limitations / planned
- **Procedural music content is hardcoded synthwave.** A planned follow-up
  (E2.1) lifts instruments / scales / progressions into a public
  `se_music_config_t` passed to `music_procedural_create()`; that will add a
  config parameter to the seed-only signature (a MINOR change).
- **Renderer cull + order are no-op seams.** `scene_render()` rasterizes in
  submission order; central frustum/back-face culling and front-to-back
  ordering are stubbed (`scene_cull_pass` / `scene_order_pass`) for a later,
  measured cut. No public-API impact when they land.
- **No object framework.** Games own their object/world pool and submit naive
  world-space geometry via `scene_tri` / `scene_line` (see `docs/objects.md`).
