# SynthEngine3D — Engine Extraction Plan

> Cross-session tracking doc for extracting the reusable engine out of the
> game and into a self-contained component at `synthengine3D/`. Work happens
> on the **`engine_extraction`** branch. This is the authoritative checklist
> for the effort — flip `- [ ]` → `- [x]` as steps land. The design
> rationale is logged in [decisions-log.md](decisions-log.md) (2026-05-23).

## Goal

Lift the game-agnostic engine (3D software renderer, object framework, audio
mixer + DSP + procedural music, vector text, UI/menu widgets, settings, save
framework) out of `main/` into `synthengine3D/`, behind a **clear,
stable public API**, so:

- engine internals can be optimized/rewritten without touching any game;
- the same component drops into other Tanmatsu apps (graceloader itself, or
  a new game) later, as an **externally-managed IDF component**;
- the game in `main/` consumes it as a component and never tracks how it is
  compiled.

The game keeps everything app-specific (the graceloader main loop, BSP
bootstrap, the app state machine, the synthwave aesthetic, gameplay, world,
objects, areas, the save *schema*, menu *content*).

## Design constraints (the contract)

1. **Dual-mode IDF component.** `synthengine3D/CMakeLists.txt` is a *real*
   IDF component when parsed by the IDF build system, and a plain CMake
   library otherwise:
   ```cmake
   if(COMMAND idf_component_register)
       idf_component_register(SRCS ${SE_SRCS}
                              INCLUDE_DIRS "include"
                              PRIV_INCLUDE_DIRS "src"
                              REQUIRES ...)            # graceloader / idf.py path
   else()
       add_library(synthengine3d OBJECT ${SE_SRCS})    # this game's plain build
       target_include_directories(synthengine3d PUBLIC include PRIVATE src)
       target_compile_options(synthengine3d PRIVATE ${SE_FLAGS})
   endif()
   ```
   An `idf_component.yml` manifest ships now (for future external
   management). This game stays a plain-CMake `.so` build and merely
   `add_subdirectory(synthengine3D)`s the vendored component — it does **not**
   become an `idf.py` project (that would fight the graceloader `.so` model:
   custom `app.ld` / `app_version.script` / `-nostdlib` / `fakelib` / crt0).

2. **Public vs. internal API, enforced by the build — three layers:**
   - **Opaque handle types.** Public headers forward-declare structs and hand
     out pointers (`typedef struct se_mixer se_mixer_t;`); the definition
     lives in `src/internal/`. The game cannot see or depend on layout, so
     internals change freely. *Exception:* value-types the API genuinely needs
     the game to see (config structs, the geometry vertex, callback vtables)
     live in public headers and **are** part of the stable contract.
   - **Include-path separation.** `include/` (public) is the only dir on a
     consumer's include path; `src/` + `src/internal/` are PRIVATE. A game
     `#include` of an internal header fails to compile — the boundary is
     mechanically enforced, not just documented.
   - **Naming + semver.** Public headers: `se_` prefix, in `include/`, with a
     "PUBLIC STABLE API — semver'd" banner. Internal: `_internal.h` in
     `src/internal/`, "INTERNAL — may change in any update" banner.
     `se_version.h` carries `SE_VERSION_MAJOR/MINOR/PATCH` + the written
     promise: public API changes follow semver; anything internal may change
     at a patch bump.

3. **Overridable defaults.** Engine ships `#ifndef`-guarded defaults
   (`SE_RENDER_HORIZON_Y`, focal length, near-clip, etc.) in `se_config.h`.
   The game overrides via (a) its own override header pulled in first, (b)
   `target_compile_definitions`, or (c) a runtime config struct passed at
   init. Hot-loop tunables stay compile-time `#define`s (see perf contract).

4. **App specifics stay in the game.** The engine is a *toolkit*, not the
   app. The graceloader main loop, BSP/display/audio bootstrap, vsync, input
   drain, and the app state machine remain in `main/`. (Native-app vs
   graceloader `#ifdef`s inside the engine are a *later* concern, out of
   scope for v1.)

5. **Self-contained documentation** lives **inside** `synthengine3D/` (it
   travels with the component; the game's `devdocs/` does not). Written for a
   developer who has never seen Race the Synth.

6. **Build green at every step.** Strangler-style migration, cleanest /
   least-coupled modules first. `make build` + `make verify` ("All symbols
   satisfied") after every step. The user commits between steps by hand.

## Performance contract

The split is a build-organization change, not a codegen change — **net-zero
at runtime if these rules hold:**

- It stays **one `.so`**; `-Wl,-Bsymbolic` binds intra-`.so` calls direct
  (no PLT). No `-flto` today, and `.c` files are already separate TUs, so no
  cross-TU inlining exists to lose. Same flags ⇒ same codegen.
- **Inline leaves stay inline in headers.** The hottest code is `static
  inline` in headers — `direct_565.h` (per-pixel/line/tri rasterizer) and
  `audio_dsp.h` (oscillators, biquad). These become **public** inline headers
  (so game HUD/text code keeps inlining them) and must never be hidden behind
  an opaque handle or a function-pointer call.
- **Opaque handles are for coarse-grained objects only** (`se_mixer_t`, the
  save-slot manager, the scene context) — things touched once per frame/event.
  Never for per-pixel / per-sample / per-triangle primitives.
- **Hot-loop tunables stay compile-time `#define`s**, not runtime struct
  fields. Runtime config is for init / once-per-frame granularity only.
- **The renderer→world inversion passes an array, not a per-element
  callback:** the shadow pass takes a `se_shadow_caster_t[]` span the game
  fills; the engine walks it (identical cost to today's pool iteration).
- **Replicate the exact compile options** in the engine target — especially
  `-march=...xesppie` (lose it and the PIE SIMD in the rasterizer silently
  degrades to scalar) and `-Os`, `-ffunction-sections`, `-fdata-sections`,
  the FreeRTOS force-includes.
- **Regression gate:** record an `app.map` size + on-device FPS baseline at
  E0; re-check after each structural move (E1, E4, E5 especially). They
  should be unchanged within noise. An FPS dip after moving a `static inline`
  header is the signal a hot leaf got de-inlined.

## Target layout

```
synthengine3D/
  CMakeLists.txt          # dual-mode (idf_component_register | add_library)
  idf_component.yml       # manifest for future external management
  README.md  CHANGELOG.md
  include/                # PUBLIC, stable — the only consumer-visible dir
    synthengine3d.h       #   umbrella (includes the public set = the TOC)
    se_version.h  se_config.h
    se_scene.h  se_direct565.h  se_text.h
    se_audio.h  se_audio_source.h  se_audio_dsp.h
    se_object.h  se_ui.h  se_save.h  se_nbt.h
  src/
    internal/             # PRIVATE headers (struct definitions, etc.)
    *.c                   # the implementations
  docs/                   # getting-started, architecture, per-subsystem guides
  examples/minimal/       # smallest app: boot, draw a tri, play a sound, exit
```

---

## Step-by-step

Each step ends with `make build` + `make verify` and is a natural commit
point. "FPS/map check" = compare against the E0 baseline.

### E0 — Scaffolding & dual-mode build (no code moves; build stays green) ✅ 2026-05-23
- [x] Create `synthengine3D/` with `include/`, `src/`, `src/internal/`, `docs/`, `examples/minimal/`.
- [x] Write the dual-mode `synthengine3D/CMakeLists.txt` (replicating the exact compile flags: `-Os -fPIC -ffunction-sections -fdata-sections -fno-common`, the `xesppie` march [via the toolchain], the FreeRTOS force-includes, `-DESP_PLATFORM`).
- [x] Add `idf_component.yml` (version `0.0.0`, description).
- [x] Add `include/se_version.h` (`SE_VERSION_*` + stability-contract comment) and `include/synthengine3d.h` umbrella.
- [x] Wire top-level `CMakeLists.txt`: `add_subdirectory(synthengine3D)`; `target_link_libraries(app_obj PRIVATE synthengine3d)` for include propagation; add `$<TARGET_OBJECTS:synthengine3d>` to the link-script JOIN + DEPENDS.
- [x] Build green with the engine effectively empty. `make verify` → All symbols satisfied. `se_version_string` is gc'd as unused (proves wiring + dead-code stripping both work).
- [x] **Baseline recorded** (see below).

**E0 baseline (`build/app.so`, `riscv32-esp-elf-size`):**

| metric | value |
|---|---|
| text | 96546 |
| data | 43580 |
| bss | 223548 |
| dec (total) | 363674 (0x58c9a) |
| on-device gameplay FPS | ~28.3 (per [README.md](README.md) parking lot; user to reconfirm on-device) |

Compare `text` / `dec` after each structural move (E1, E4, E5) — should be
unchanged within noise. An FPS dip after an `E1`/`E4` move signals a hot
`static inline` leaf got de-inlined.

**On-device A/B confirmation (2026-05-23, after E0–E2).** Compared the
per-frame stage breakdown before the split vs. the current build under the
*same* (heavy-banner) content. Raw FPS is content-driven and swings 12→23
in both builds, so the stable per-frame stages are the real signal — and
they match within run-to-run jitter: `in`~0.07, `phys`~0.73, `bgkick`~10.3,
`bgflr`~14.7, `fgrest`~3.5, `blit`~0.54, `vsync`~0. The decisive one is
**`fgrest`~3.5 ms in both** — that path renders HUD + sign text through the
moved `se_text.h`/`se_direct565.h` inline leaves, so an unchanged `fgrest`
proves the inlining survived the component boundary. `obs` swings
identically in both (banner triangle/Hershey-stroke content cost, not the
split). Audio (E2) doesn't appear in render-thread timings — it mixes on
its own task. **Conclusion: the extraction so far is performance-neutral.**

### E1 — Tier-1 leaf primitives (hot; must stay inline) ✅ 2026-05-23
- [x] `direct_565.h` → `include/se_direct565.h` (PUBLIC inline; every `static inline` kept). 5 game includers updated (`render.c`, `scene.c`, `main.c`, `synthwave.c`, `objects/bridge.c`); `#include "magicnumbers.h"` → `#include "se_config.h"`.
- [x] `hershey.h` / `hershey_font.h` / `hershey_font_direct.h` → `src/internal/` (INTERNAL); `rendertext.c` → `src/`; `rendertext.h` → `include/se_text.h` (PUBLIC). The `simplex[]` table stays a single definition in `rendertext.c`'s TU (now an engine TU); exposed to the one external consumer (`objects/synthengine_sign.c`) via a documented `extern int simplex[95][112]` in `se_text.h` (replaced the bare extern).
- [x] **New `include/se_config.h`** seeded with the 5 display-geometry constants (`DISPLAY_RAW_W/H`, `DISPLAY_LOG_W/H`, `DISPLAY_RAW_STRIDE`) as `#ifndef`-guarded overridable defaults — pulled forward from the E4 magicnumbers split because `se_direct565.h` needs them. `magicnumbers.h` now `#include`s `se_config.h` so game code still sees the values.
- [x] Engine CMakeLists: `src/rendertext.c` added to `SE_SRCS`; `src/internal` added to the (priv) include dirs. Top-level `APP_SOURCES` drops `main/rendertext.c`. Umbrella `synthengine3d.h` now includes `se_config.h` / `se_direct565.h` / `se_text.h`.
- [x] Build + verify + **map check passed: byte-identical to E0 baseline** (text 96546 / dec 363674 — hot inline leaves still inline; zero codegen change). All symbols satisfied. Boundary checks clean: no game source includes Hershey internals; no engine source includes `magicnumbers.h`. (On-device FPS unchanged expected; user to reconfirm on next flash.)

### E2 — Audio stack (clean, zero game coupling) ✅ 2026-05-23
- [x] `audio_source.h` → `include/se_audio_source.h` (the `music_source_t` / `sfx_voice_t` vtable contracts — public value-types; games implement them).
- [x] `audio_dsp.{c,h}` → `include/se_audio_dsp.h` + `src/audio_dsp.c`; the synth-primitive `static inline`s stay public.
- [x] `audio_mixer.{c,h}` → `include/se_audio.h` + `src/audio_mixer.c`. (No opaque `se_mixer_t` — the mixer is a global singleton with free functions, so there's no per-instance handle to hide; the *source/voice* structs are the exposed contract.)
- [x] `music/music_procedural.{c,h}` → `include/se_music_procedural.h` + `src/music_procedural.c`. Signature kept `music_procedural_create(seed)` for now — see **E2.1**.
- [x] **SFX seam:** the mixer/source/DSP/procedural-music framework is engine; the *specific sound recipes* (`sfx/*`) stay game-side and call the engine DSP. Confirmed and documented.
- [x] **Coupling 1 inverted — settings gate.** The mixer used to call `audio_settings_*()` (game NVS) to gate music/SFX/hum. Replaced with a **push model**: the mixer holds gate state and exposes `audio_mixer_set_music_enabled()` + `audio_mixer_set_group_enabled(group, on)`; the game's `audio_settings.c` (set_*) and `main.c` (startup) push the values. Engine now has zero app/NVS dependency.
- [x] **Coupling 2 inverted — gains.** `AUDIO_MUSIC_GAIN` / `AUDIO_SFX_GAIN` moved from `magicnumbers.h` to `se_config.h` as overridable defaults (only the mixer used them in code).
- [x] **Hum de-leaked (raised by user).** The engine no longer hardcodes a `SFX_VOICE_TAG_HUM` class. `sfx_voice_t` now carries a generic `uint8_t group`; the mixer gates by group (`SE_AUDIO_SFX_GROUP_COUNT` groups, default-enabled) and assigns no meaning to any group. The *game* defines `AUDIO_SFX_GROUP_GENERAL=0` / `AUDIO_SFX_GROUP_HUM=1` (in `audio_settings.h`) and tags only the hum voice. So the hum *sound* and its *meaning* are entirely game-side; the engine just provides generic mute groups.
- [x] Build + verify: **All symbols satisfied.** Size text 96546→96662 (+116 B: the generic group array + bounds-check + setter — added functionality, not a hot-path regression; audio runs off the render loop). Boundary checks clean (no engine include of `magicnumbers.h`/`audio_settings.h`; no `SFX_VOICE_TAG`/`tag` left anywhere).

### E2.1 — Parameterize the procedural music generator (planned)
Raised by user 2026-05-23: the generator is engine-side, but its musical
*content* (instruments/voices, scales/modes, chord progressions,
tempo/structure) is hardcoded as a synthwave personality, so today it is
reusable only as "a synthwave generator." Lift the content into a public
`se_music_config_t` passed at create time —
`music_procedural_create(const se_music_config_t* cfg, uint32_t seed)` —
so other games drive the same generator with their own music. The current
synthwave settings become the game's data (or an engine-provided
`se_music_synthwave_preset()` default). Its own reviewable change because
designing the config schema is the bulk of the work; deferred so E2 stays
a faithful relocation.

### E3 — Serialization + save framework split ✅ 2026-05-23 (Option B, co-designed)
- [x] `nbt.{c,h}` → `se_nbt.{c,h}` (generic FILE*-based tagged serializer, public; banner added).
- [x] **Generic slot framework in the engine (`se_save.{c,h}`)** — chose **Option B** over the minimal "NBT only" cut, because reuse is the explicit goal and the user listed "save file handling" as an engine capability. The engine owns: N file-backed slots (`SE_SAVE_SLOT_COUNT`, overridable `#define` in `se_config.h`), the on-disk wrapping (NBT root + an engine **peek header**), the slot directory (`mkdir`), and `slot_exists`/`peek`/`load`/`write`. The game provides a `se_save_config_t` (dir, game name, game version, serialize/deserialize callbacks). API: `se_save_init` / `se_save_slot_exists` / `se_save_peek` / `se_save_load_slot` / `se_save_write_slot(slot, kind, data, info)`.
- [x] **Peek header (co-designed):** engine fills `timestamp` + `format_version` automatically; the game supplies `game_name`/`game_version` (config) + a free-text `info` display string + a `se_save_kind_t` (MANUAL/AUTOSAVE/QUICKSAVE — generic, RTS always MANUAL) per write. Written as the `se_peek` compound first in the file so slot-select reads it without loading full state.
- [x] **Game side stays, thinner:** `save_nbt.c` dropped `write_peek`/`read_peek`; `save_write_state`/`save_read_state` are now the two callbacks (they skip the engine's `se_peek` as an unknown tag). `save.c` builds the config + the `info` summary string (`"best… stage… runs…"`) and delegates to `se_save_*`; `save_data_t` lost its 4 peek-mirror fields and `save_peek_info_t`/`save_slot_peek` are gone. `main.c` slot-select reads `se_save_peek_t` (timestamp + `info`).
- [x] **Deviation from the original sketch:** day-rollover stayed game-side (it's daily-challenge *policy*, not generic persistence), and there's no opaque handle (the slot manager is a configured singleton). Recorded in decisions-log.
- [x] Build + verify: **All symbols satisfied.** Size text 96662→97063 (+401 B: the generic peek + config + callback layer — added functionality, not a hot path; save runs on menu transitions). Boundary clean: engine save sources include no game headers; no stale peek refs in the game.
- [ ] **On-device smoke still needed (format changed):** confirm (a) a pre-existing slot still loads its progress and shows blank date/info until re-saved, then re-saves correctly; (b) a fresh save writes → loads → peeks round-trip. *(Can't be done from the build host.)*

### E4 — 3D scene renderer + invert the world dependency ✅ 2026-05-23
- [x] `scene.{c,h}` → `se_scene.{c,h}` (rasterizer + camera + projection; symbol names kept — `scene_init/begin/tri/line/flush`, `render_camera_t`, `render_set_camera`/`render_camera`/`render_project`). Banner added; `#include "magicnumbers.h"` → `se_config.h`.
- [x] **Camera + projection moved from `render.c` into `se_scene.c`** (declared in `se_scene.h`). **The "inversion" resolved by recognition rather than a `se_shadow_caster_t` abstraction:** `render_submit_obstacles` / `render_shadows` (and the cube/pyramid/icosahedron emitters) are *game* code that walks the obstacle pool, so they stayed in the game's `render.c`, now calling the engine's `scene_tri`/`scene_line`/`render_project`. No premature caster-array API was needed — the floor-shadow projection is game-specific (sun angle, synthwave floor). Result: **the engine no longer includes `world.h`** (the whole point of E4).
- [x] **Magnumbers split (finished):** `RENDER_HALF_W/HORIZON_Y/FOCAL_LEN/CAM_Y/NEAR_CLIP_Z` → `se_config.h` as `#ifndef` overridable defaults (with `RENDER_HORIZON_Y` documented as the per-game backdrop-match knob). Combined with E1 (display geometry) + E2 (audio gains), the engine half of `magicnumbers.h` is now all in `se_config.h`; `GAME_*` gameplay tuning stays game-side. `render.h` re-exports `se_scene.h` so all existing call sites keep compiling unchanged.
- [x] Build + verify + **map check passed: text 97063→97109 (+46 B, just the relocation).** All symbols satisfied. **Boundary confirmed: no engine source/header includes `world.h`/`render.h`/`game.h`/`magicnumbers.h`.** Bonus: `scene_project` (per-vertex hot path) now shares a TU with `render_camera()` so it can inline that call — marginally *better* than the pre-split cross-TU call. (On-device FPS reconfirm welcome on next flash, but the +46 B and the inlining argument predict no regression.)

### E5 — Genericize the object/emit framework
- [ ] `obstacle.{c,h}` → `se_object.{c,h}`: pool + callback dispatch (`emit/collide/shadow/physics/cleanup` + scratch). Generalize `kind`: engine = "user object + callbacks"; the game defines and registers its kinds.
- [ ] Migrate `objects/*` to the engine object API (they remain game content).
- [ ] Build + verify + FPS check.

### E6 — UI / menu widget framework
- [ ] Extract `menu_view_t` + `menu_draw()` + row kinds + panel/chevron drawing from `main.c` → `se_ui.{c,h}`. Game keeps menu content + the state machine.
- [ ] Build + verify; on-device menu smoke.

### E7 — Backdrop split (scope decision)
- [ ] Decide: extract a generic PPA hardware-blit helper to the engine, keep the synthwave shapes/aesthetic game-side — **or defer** (keep `synthwave.c` entirely game-side for v1). Flagged optional.
- [ ] Build + verify (if anything moves).

### E8 — Documentation (first-class deliverable)
- [ ] `README.md` — pitch, capabilities, "hello triangle + sound" quick start, both build modes, version/stability policy, links into `docs/`.
- [ ] `CHANGELOG.md` — semver'd (companion to `se_version.h`).
- [ ] `docs/`: `getting-started.md`, `architecture.md` (subsystems, frame lifecycle, callback/IoC model, public vs internal), `renderer.md`, `audio.md`, `ui.md`, `save.md`, `objects.md`, `configuration.md`, `integration.md`.
- [ ] Migrate the game's `devdocs/importing-objects.md` content → `synthengine3D/docs/objects.md`; leave a pointer in the game docs.
- [ ] `examples/minimal/` — smallest documented app.
- [ ] Document the public/internal boundary, the semver contract, and the inline-leaf / opaque-coarse performance rule for engine users.

### E9 — Final verification & polish
- [ ] Full `make clean build` + `make verify` (all symbols satisfied).
- [ ] `app.map` size vs E0 baseline (≈ unchanged); on-device FPS vs baseline (≈ unchanged).
- [ ] Grep-lint: no game source includes any engine `*_internal.h`.
- [ ] On-device full-playthrough smoke (render, audio, save, menus, objects).
- [ ] Update the game's `devdocs/architecture.md` + `README.md` status to reflect the engine boundary.

---

## Open decisions (resolve as we reach them)

- **SFX ownership** (E2): framework engine-side, recipes game-side — confirm
  on contact.
- **Object struct visibility** (E5): the object carries game-set callback
  pointers + scratch, so it's likely a *public* value-type with an
  engine-owned pool, rather than fully opaque. Decide at E5.
- **Backdrop / PPA** (E7): extract the blit helper vs. defer entirely.
- **Native-app vs graceloader `#ifdef`s** inside the engine — explicitly
  deferred past v1.
