# Roadmap — Phases, Verification & Key Files

> The phased implementation plan, per-phase verification checklist and the critical-files reference. Part of the [dev docs](README.md).

## Critical Files & References

Files to **modify** (this repo):

- `main/main.c` — replace demo content
- `main/crt0.c` — unchanged
- `CMakeLists.txt` — extend `APP_SOURCES`
- `metadata/metadata.json` — name "Race the Synth" (currently
  "Race the Synth GL"), version 0.1.0, description, author already set

Files to **create** (this repo):

- `main/{game,world,render,meta,audio,input}.{c,h}`
- `main/synthwave.{c,h}` (copied + trimmed)
- `main/sfx.h` (generated from PCM with `xxd -i`)

Key reference files (read-only, for patterns):

- `tanmatsu-launcher/main/synthwave.{c,h}` — the synthwave backdrop +
  scrolling grid (lines 15–270 are the pure-render functions we want; the
  animator task at 272–392 is *not* needed since we own the loop).
- `tanmatsu-launcher/main/global_event_handler.c:19–67` — the exact
  volume up/down handling pattern (NVS namespace `"system"`, keys
  `"speaker.volume"` / `"hp.volume"`, `bsp_audio_set_volume(percent)`).
- `tanmatsu-floppybird-grace/main/main.c:139–230, 437–442, 509–527,
  540–750` — game-loop, vsync, NVS high-score, audio task patterns.
- `tanmatsu-placeinvaders-grace/main/{game,render,audio}.c` — multi-file
  layout, sprite/sound embedding.
- `include/pax_gfx.h, pax_text.h, shapes/pax_tris.h, pax_matrix.h` — the
  PAX 2D drawing API.
- `include/bsp/{display,input,audio,led}.h` — BSP entry points.
- `include/nvs.h, nvs_flash.h` — persistence API.

---

## Implementation Phases

Done in this order so each phase produces a runnable build:

1. **Skeleton**: extend CMakeLists, create empty `.c/.h` stubs, copy and
   refactor synthwave into the layered draw functions, run
   `synthwave_init()` to pre-triangulate, render sky → sun → mountains →
   wireframe → top-grid → bottom-grid + a "Press F1 to exit" prompt. Build,
   install, run. Validates the build/install loop and the pre-triangulation
   path (frame must hit ≥30 FPS already at this stage).
2. **Ship + steering**: render a triangular ship at the bottom centre,
   wire left/right polled input, integrate `vx` with friction. No
   obstacles. F1 exits.
3. **3D projection + obstacles**: implement `project()`. Spawn a single
   stream of cuboid obstacles at fixed lateral lanes, advance them toward
   the camera. Render as 1-colour triangles, painter-sorted by z.
   Validates the 3D pipeline.
4. **Collision + game over**: AABB overlap test against every
   active entry in the obstacle pool (walls + dynamic obstacles
   both). For each overlap, classify by smallest-axis penetration:
   x-axis contact ⇒ scrape (set flag, *gradually* ramp
   `ship_speed_z` down at a fixed deceleration toward a scrape-
   floor ~0.5-0.6 × base while contact lasts; when contact ends,
   *gradually* ramp back up at a separate acceleration toward
   base speed — neither edge is a step change); z-axis contact ⇒
   head-on ⇒ STATE_GAME_OVER → back to STATE_TITLE on
   space/gamepad-A. Ship's collision AABB derived from the
   tetrahedron extents at `SHIP_Z_PLANE` (roughly `half_w ≈ 0.28`,
   `half_d ≈ 0.34`, `height ≈ 0.30`). State machine lands here too —
   `STATE_TITLE` / `STATE_PLAYING` / `STATE_GAME_OVER`, with
   `input_set_mode` driving the modal ESC/Backspace bindings.
   Scrape sparks also land in this phase — fixed-size particle
   pool emitting at the scraping wing-tip, world-anchored so
   they drift back at `ship_speed_z`, projected through the
   same camera as obstacles (see the 2026-05-11 decisions entry
   for the model).
5. **Sun timer + shadow + boost pickups**: tick `sun_seconds_left`, sink
   the visual sun (verify it disappears behind the mountain silhouette),
   end run on sunset. Implement `is_ship_in_shadow()` and the speed
   slowdown. Spawn boost pickups; collect → +time and speed reset.
6. ✅ **Tris + multiplier** (landed 2026-05-14). Three-stream
   scoring: per-frame `distance × multiplier × 0.1` (mirrors the
   original RTS distance income, with the 0.1 factor scaling
   it down so the pickup bumps stay perceptible) plus Tri
   pickup (5 × mult) and booster pickup (10 × mult) bonuses,
   all sharing the same `multiplier`. `pickups_tri` is
   monotonic per run; the top-left opaque grey HUD panel
   shows four Tri slots filling left-to-right (lit-count =
   `pickups_tri % 5`) plus the current multiplier — the 5th
   pickup ticks the multiplier and resets the row in the
   same frame. New `sfx_pickup_plink` plays one of five
   ascending pentatonic pitches (C5/D5/E5/G5/A5) keyed to
   the in-cycle slot index, so the 5 pickups of a cycle
   form a brief musical phrase that resolves on the 5th
   note (the audio cue for the multiplier bump). New
   `objects/tri.c` is a copy of the original
   `objects/booster.c` pyramid, recoloured cyan-blue; the
   booster itself moves to a slowly-rotating regular
   icosahedron in `render_booster_icosahedron` (12 verts /
   20 faces / 30 edges, ~1 Hz Y-axis spin, faceted-gem
   shading with face-normal lighting + edge wireframe).
   Per-area spawn rules (all per the locked 2026-05-14
   spec): pixel_field half-count + big_blocks equal-count
   via rejection-sampling against the live obstacle pool
   (new `world_find_free_x` helper), gateways +
   dynamic_gateway fill the non-booster hole, dynamic_passage
   emits one per flipping cube in the clutter (own
   safe-lane-aware emit_tri), bridges co-spawn one per
   bridge along a random-angle line picked at area init,
   rest area between stages lays 10 Tris + 1 booster on a
   quadratic-Bézier S-curve (P0=P1 in x for zero tangent at
   t=0); pre-stage-1 lead-in deliberately exempted. Crash
   penalty (-5 to multiplier, floor 1) wired in
   `game_collide`. Per-run + all-time Tri totals persist
   automatically via the existing `run_stats` merge. See
   the 2026-05-13 / 2026-05-14 Phase 6 decisions-log
   entries for the full reasoning trail.
7. ✅ **Audio** (landed 2026-05-13). Diverged from the original
   "embed PCM via xxd" plan: SFX are procedurally generated by
   shared DSP primitives (`main/audio_dsp.{c,h}` — oscillators,
   ADSR, biquad), music is a procedural synthwave generator
   seeded from a per-run `music_prng` split from the world seed,
   and the mixer (`main/audio_mixer.c`) talks to sources through
   a swappable `music_source_t` trait so a future `.mod` /
   MP3 / MIDI backend slots in without touching the mixer.
   Master gains in `magicnumbers.h` set the music-vs-SFX balance
   with headroom for 5 concurrent SFX. Volume keys + audio-jack
   hot-swap go through the upstream `nvs_settings_*` module
   (cherry-picked from `tanmatsu-template-grace`) so the values
   match the launcher's. Pause stops SFX but keeps music. See
   the 2026-05-13 decisions-log entries for the full story
   (Phase 7 design, master volume integration, audio first-light
   + tuning).
8. **Daily seed + custom seed + persistence**: read RTC, derive daily
   seed, persist highscore/level/points to NVS namespace `synthracer`.
   Use `last_seen_date` only to detect day-rollover (reset daily-challenge
   completion) — no clock-rollback anti-cheat (offline single-player game).
   Add the title-screen "Custom Seed…" entry dialog + `last_custom_seed` /
   `cs_best` persistence. Custom-seed runs award meta-progression
   identically to daily runs.
9. **Pickups & attachments.** Built **ungated** — pickups always
   spawn and attachments are always equippable; the `unlock_*`
   gating is wired later in Phase 11, and storage caps
   (double/triple jump, etc.) are plain constants for now. Split
   into five sub-phases, each a runnable build:
   - **9.1 — Vertical system + Jump pickup + ramps.** Adds the
     ship's vertical dimension and everything that depends on it —
     see the 2026-05-15 "Phase 9 sub-phases + 9.1 vertical-system
     design" decisions-log entry for the full spec. In short:
     `ship_y` + `ship_vy` + gravity in `game_step`; a shadow ray
     replacing the floor-pixel `in_shadow` sampler; Y-aware
     (x-y-z) collision; landing on / riding obstacle tops; the
     border-wall collision→clamp redesign; the jump pickup
     (explicit `vy` injection); and ramps (visible wedges,
     emergent launch from slope × speed). Jump and ramps ship
     together — a ramp is a sloped platform and needs the whole
     system.
   - **9.2 — Shield pickup.** Single-use; absorbs one head-on hit
     instead of dying — brief invuln + forward/up teleport.
   - **9.3 — Checkpoint pickup.** ✅ done. Stores a respawn point;
     on a head-on crash, rewind there instead of GAME_OVER. Design
     resolved (see decisions-log 2026-05-19 / 2026-05-22): the
     snapshot is a struct copy of `world_state_t` + `game_state_t`;
     on crash the **level** rewinds (generation, RNG, ship, sun,
     inventory) but the player's **run progress is kept** (score,
     distance, multiplier+peak, pickup tallies carry forward), so a
     Re-Do never costs accumulated progress or meta-progression
     credit.
   - **9.4 — Attachment slots + Magnet.** The equip framework
     (`attach1`/`attach2`, fills the `APP_STATE_UPGRADE_STUB`
     screen) plus the Magnet attachment — pulls nearby pickups
     toward the ship within a radius.
   - **9.5 — Battery upgrade.** Needs a short design pass; likely
     extends the sun/power budget. The ship model already carries the
     hardware readout: a battery panel + four charge-indicator regions
     (`SHIP_REGION_BATTERY_PANEL`, `SHIP_REGION_INDICATOR_0..3` in
     `objects/ship_model.h`). This phase drives each indicator's colour
     individually from charge state and skips drawing the panel +
     indicators when no battery is installed (the hook is already in
     `game_submit_ship`).
10. **Regions**: ✅ dissolved 2026-05-13. Content variation now
    rides on stages + area types rather than a discrete 7-region
    overlay — new areas (e.g. bridges, dynamic_passage,
    dynamic_gateway) supply their own palettes / mutators /
    stage-gate ranges; the picker pulls from whatever's applicable
    per stage. Per-run challenge flags that this phase would have
    owned (perfect-region, movement-restriction) move into Phase
    11's metaprogression tracking instead.
11. **Meta-progression**: level table, 3-slot challenge system,
    challenge templates, level-up SFX & banner, unlock applications.
12. **Apocalypse mode** (lv 11): faster speed, denser obstacles,
    different region tints. Reuse the same render path.
13. **Polish**: title screen art, game-over splash with daily challenge
    summary, LED accents on side LEDs (e.g. red on near-collision flash,
    green on multiplier-up), brightness via F2/F3.

Phases 1–8 produce the **MVP** — playable game with persistent score,
working volume, daily seed. Phases 9–13 deliver the full original-game
experience.

---

## Verification

For each phase:

1. **Build**: `make clean build` from project root — single command, no
   manual IDF env setup. **Do not** source `$IDF_PATH/export.sh` or any
   other ESP-IDF export script directly; the Makefile handles all env
   plumbing internally. Build must produce `build/app.so` without errors
   and `make verify` must pass (all symbols satisfied by
   `fakelib/liball.so`). Inspect `build/app.map` if size grows
   unexpectedly.
2. **Install + run**: `make install run` (assumes Tanmatsu connected on
   `/dev/ttyACM0` and `tanmatsu-launcher` running graceloader). The app
   should launch from the loader and render the synthwave backdrop. F1
   returns to the launcher.
3. **Smoke checklist** per phase:
   - Phase 1: backdrop draws, grid scrolls, F1 exits.
   - Phase 2: ship moves left/right, no jitter, friction feels sane.
   - Phase 3: obstacles approach and pass under the ship.
   - Phase 4: ship crashes on contact, game-over screen appears, space
     restarts.
   - Phase 5: sun visibly sinks; boosts raise it.
   - Phase 6: score increases; collecting 5 Tris bumps the multiplier.
   - Phase 7: SFX play; VOLUME_UP/VOLUME_DOWN change volume; quitting to
     launcher and changing volume there shows our change persisted (read
     `system/speaker.volume` from launcher settings UI).
   - Phase 8: power-cycle the device; persisted highscore reappears;
     advancing the system clock by one day reshuffles the world.
   - Phase 9–13: tested as features land.
4. **Daily seed determinism**: run the game twice on the same day → same
   obstacle pattern in region 1. Force-set the date forward one day →
   different pattern.
5. **Custom seed determinism**: enter a fixed custom seed twice; verify
   the world is identical (same obstacle/pickup placement region by
   region). A custom-seed run awards `points` / `level` exactly like a
   daily run — confirm meta-progression advances normally.
6. **Sun occlusion**: drain the sun timer manually (or watch a long
   run) and confirm the sun visually slides behind the mountain
   silhouette rather than being clipped at a hard edge.
7. **Shadow speed loss**: stand the ship behind a tall obstacle and
   confirm `ship.speed` drops, then dodge clear and confirm it
   re-accelerates.
8. **Volume persistence**: change volume in our app, exit to launcher,
   confirm launcher volume bar reflects the new value (launcher reads
   the same NVS keys).
9. **Memory**: check `make build` size output stays under ~3 MB so it
   loads on graceloader's mapped window. PSRAM allocation for any
   secondary buffer (and the cached triangulation index arrays) should
   use `MALLOC_CAP_SPIRAM`.

No automated test suite is in scope — Tanmatsu graceloader apps are
validated on-device.

---

