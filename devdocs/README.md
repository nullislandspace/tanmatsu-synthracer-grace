# Race the Synth — Development Docs

Canonical, cross-session documentation for **Race the Synth**, a Tanmatsu
graceloader game (a synthwave-reskinned clone of *Race The Sun*).

This was originally one monolithic `DEVELOPMENT.md`; as it grew past ~4500
lines it was split into the topic documents below. No content was lost in
the split — each document is a verbatim slice of the original, plus this
overview. The repo-root `DEVELOPMENT.md` is now just a pointer here.

## Documentation index

| Document | Contents |
|----------|----------|
| [decisions-log.md](decisions-log.md) | Append-only chronological log of every design / implementation decision. The project history. |
| [architecture.md](architecture.md) | Project context, file layout, and per-module responsibilities (`game.c`, `render.c`, `scene.c`, `world.c`, …). |
| [importing-objects.md](importing-objects.md) | How-to: turn an OpenSCAD model into an in-game world object via `tools/object_3mf_to_header.py` (modelling conventions, converter config, the emit callback, wiring). |
| [roadmap.md](roadmap.md) | The phased implementation plan, per-phase verification checklist, and critical-files reference. |
| [performance.md](performance.md) | Frame-time profiling and the catalogue of viable FPS optimisations. |
| [input-mapping.md](input-mapping.md) | Keyboard / gamepad bindings and the rationale behind them. |
| [research.md](research.md) | Appendix A — offline-cached research on the original *Race The Sun*. |
| [api-reference.md](api-reference.md) | Appendix B — cached cheat-sheet of the Tanmatsu SDK APIs in use. |

The **Current Status** table below is the authoritative summary of what is
done and what is next.

## Current Status

**Phase: Phases 1–8 + 10 complete; Phase 9 in progress. Phase 8 (daily seed + custom seed + persistence) closed 2026-05-15 — the game is playable end-to-end with a daily/custom seed and persistent per-slot scores, which is the MVP bar. The meta-progression layer (challenge system, levels, points, unlocks) was re-scoped out of Phase 8 entirely and now lives in Phase 11. Phase 9 (pickups + attachments) is split into sub-phases: 9.1 (the vertical system — ship altitude, gravity, jumps, Y-aware collision, landing/riding, shadow ray, moving camera, jump booster, ramps, simple_platform area) is ✅ complete as of 2026-05-16; 9.2 (shield pickup — generalised owed-pickup API, once-per-stage spawn from stage 3, 4 s invulnerability window, ship aura, HUD) is ✅ complete as of 2026-05-19; 9.3 (checkpoint pickup — black/white icosahedron, once-per-stage spawn from stage 5, snapshot + on-crash **level rewind that preserves run progress** — score/distance/multiplier/pickup tallies carry forward, "Re-Do from checkpoint" dialog, gong SFX, HUD) is ✅ complete as of 2026-05-19 (progress-keeping revision 2026-05-22). Remaining in Phase 9: 9.4 attachment slots + magnet, 9.5 battery. Phase 11 (meta-progression UI) is the next persistence/UI phase.**

| Phase | Description | State |
|-------|-------------|-------|
| 0 | Research, design, plan | ✅ done — see this document |
| 1 | Skeleton + synthwave backdrop | ✅ runs on-device; sun arch fixed, Hershey font, F-key icon hint, horizon lifted, grid floor extended with extra perspective lines on each side |
| 2 | Ship + steering | ✅ runs on-device. Ship is now an **imported OpenSCAD model** (`main/objects/ship_model.h`, generated from `openscad/ship.3mf` by `tools/ship_3mf_to_header.py`): gold per-face-lit hull, black battery panel with four white charge-indicator cells, plus two magnet poles (red/blue) — all the non-body parts drawn flat with no outline. Partitioned into regions (`ship_region_t`) so the panel + indicators are ready for the Phase 9.5 battery module and the magnet poles for the Phase 9.4 magnet attachment. (Earlier it was a placeholder delta-wing.) See the 2026-05-22 ship decisions-log entries. |
| 3 | 3D projection + obstacles | ✅ runs on-device; floor stripes + lanes + obstacles share one pinhole projection; obstacles render as 3D cubes with per-face culling and near-plane clipping; continuous side walls along the track edges, all stored in the same obstacle pool ready for collision in Phase 4 |
| 4 | Collision + game over | ✅ TITLE → PLAYING → GAME_OVER state machine; AABB collision against the unified obstacle pool with a boundary-obstacle position rule (scrape on side walls, head-on on any playfield obstacle ahead); push-out resolution + continuous scrape-floor/recovery speed dynamics; per-frame red radial-burst sparks at the banked wingtip while scraping |
| 5 | Sun timer + shadow + boost | ✅ done — sun, shadows, stall, full-sunset, boost pickups all landed |
| 6 | Tris + multiplier | ✅ done 2026-05-14 — distance × multiplier × 0.1 per-frame income (from the original RTS model, scaled so pickup bonuses stay visible) + Tri pickup bonus (5 × mult) + booster pickup bonus (10 × mult) all stacking; monotonic `pickups_tri` counter (HUD reads `% 5`); top-left opaque grey HUD panel with 4-slot Tri progress row + multiplier text (5th Tri ticks the multiplier and resets the row); ascending pentatonic 5-note plink SFX (`sfx_pickup_plink.c`, the 5th note doubles as the multiplier-bump cue); Tri object copied from `objects/booster.c` into `objects/tri.c`, painted blue; booster shape upgraded pyramid → rotating regular icosahedron (12 verts / 20 faces / 30 edges, ~1 Hz spin around Y, faceted-gem rendering with face-normal lighting + edge wireframe in `render_booster_icosahedron`); per-area spawn rules implemented (pixel_field half-count / big_blocks equal-count via rejection sampling against the live pool, gateways + dynamic_gateway fill the non-booster hole, dynamic_passage emits one per flipping cube in the clutter, bridges co-spawn one per bridge along a random-angle line, rest area between stages lays 10 Tris + booster on a quadratic-Bézier S-curve, pre-stage-1 lead-in deliberately excluded); per-run + all-time Tri totals persist automatically via the existing `run_stats_merge_into_all_time`. Crash penalty (-5 to multiplier, floor 1) wired in `game_collide`. New `world_find_free_x` helper for shared rejection sampling. See the 2026-05-13 / 2026-05-14 Phase 6 design-update + implementation decisions-log entries. |
| 7 | Audio + volume keys | ✅ done 2026-05-13 — software mixer (22050 Hz s16 stereo, swappable music-source interface), procedural synthwave music generator seeded from a separate `music_prng`, five procedurally-generated SFX (engine hum, pickup ding, crash, scrape, cube bump), audio settings (music/SFX/hum toggles in `synthracer` NVS), master gain staging in `magicnumbers.h`, master volume + audio-jack hot-swap via the upstream `nvs_settings_*` module, pause-stops-SFX-music-keeps-going wiring. F2/F3 live brightness step keys deliberately deferred (boot-time loading covers the "honour the launcher" half). See the 2026-05-13 audio decisions-log entries. |
| 8 | Daily + custom seed + persistence | ✅ done 2026-05-15 — RTC-derived daily seed (`year*10000 + month*100 + day`), now sourced from a single `s_session_date` snapshot captured once at boot so "today" is frozen for the whole session. Custom-seed entry dialog + `last_custom_seed` prefill/persist. Persistence redesigned 2026-05-12: 3 explicit save slots as NBT files in `/int/synthracer/save{0,1,2}.bin` (NVS dropped), explicit-boolean unlock + daily-done flags. Slot selection on boot, main menu, seed-input subscreen, stats screen, basic scoring + per-slot `last_run`/`all_time` stats, correct `SAVE_END_*` reason recording, and day-rollover detection (`save_apply_day_rollover`) all landed. **Scope note:** the meta-progression layer originally sketched under this phase — the challenge system that *writes* the `daily_done_*` flags, plus level/points/unlock logic — moved wholesale to Phase 11; the save struct carries all the fields but no gameplay code reads/writes them yet. The game is playable with a daily seed + persistent per-slot scores, which is the MVP bar. |
| 9 | Pickups & attachments | ⬜ not started — split into sub-phases below; built **ungated** (`unlock_*` gating wired later in Phase 11). |
| 9.1 | Vertical system + Jump pickup + ramps | ✅ done 2026-05-16 — all nine sub-milestones landed: **a** (`ship_y`/`ship_vy`/gravity + jump), **a.5** (moving camera + `render_camera` global), **b** (Y-aware collision), **c** (landing/riding obstacle tops), **d** (geometric shadow ray), **e** (border-wall clamp, edge-aware), **f** (jump booster + inventory + HUD), **g** (ramp object + emergent launch), **h** (simple_platform test area). One parked tuning item: the ramp→platform arc alignment. See the 2026-05-15/16 "Phase 9 sub-phases + 9.1 vertical-system design" and the two "Phase 9.1 implementation" / "Phase 9.1 complete" decisions-log entries. |
| 9.2 | Shield pickup | ✅ done 2026-05-19 — violet hexagonal-plate pickup (`objects/shield.c`); generalised owed-pickup API (`area_owed_t` + `owed_pickups[]` + the `world_place_pickup` helper routing every area's Tri slot, booster left on its own per-area placement); one shield scheduled per stage from stage 3; collection banks a charge (cap **1**); a head-on crash spends it for a 4 s invulnerability window (crash SFX + sparks still fire, debounced); spinning violet hexagon aura around the ship while active; HUD hexagon; pre-stage-1 lead-in spawns a shield instead of a jump booster. See the 2026-05-19 Phase 9.2 decisions-log entries. |
| 9.3 | Checkpoint pickup | ✅ done 2026-05-19 (progress-keeping revision 2026-05-22) — black-and-white icosahedron pickup (`objects/checkpoint.c`, a soccer-ball reskin of the speed booster); scheduled once per stage from stage 5 via the owed-pickup API; collecting one plays a procedural gong (`sfx/sfx_gong.c`) and snapshots the run state (a struct copy of `world_state_t` + `game_state_t` — obstacle pool, area, RNG, wall cursors, ship, sun, inventory). A head-on crash with a checkpoint (and no shield) **rewinds the level** to that snapshot but **keeps the player's run progress**: `score`, `distance_traveled`, `multiplier` (+ peak) and the five `pickups_*` tallies carry forward from the crash-time state, so a Re-Do never costs accumulated progress or the meta-progression credit they feed at run end (max stage reached `s_peak_stage` and play-time `s_run_play_seconds` are main.c statics, already untouched by the rewind). It opens the pause-like **`APP_STATE_CHECKPOINT_REDO`** dialog ("Re-Do from checkpoint" + Churchill quote, scene frozen behind, music keeps playing); space resumes — checkpoint spent, shield window granted. HUD: a black/white checkerboard. Built **ungated**. See the 2026-05-19 and 2026-05-22 Phase 9.3 decisions-log entries. |
| 9.4 | Attachment slots + Magnet | ⬜ not started |
| 9.5 | Battery upgrade | ⬜ not started |
| 10 | Regions | ✅ dissolved into the stage + area system — content variation is added incrementally as stages and area types land (the recent flipping_cube / dynamic_passage / dynamic_gateway additions are concrete examples), rather than as a discrete 7-region table cut-in |
| 11 | Meta-progression UI | ⬜ not started — **scope expanded:** absorbs the meta-progression work originally filed under Phase 8. The `meta.c` module: 25-level unlock ladder, 3-slot daily challenge system + challenge templates, awarding challenge points / level-ups, applying unlocks, level-up SFX & banner. Also owns the daily-challenge half of day-rollover — `save_apply_day_rollover()` already clears the `daily_done_*` flags on a day change (Phase 8), but the flags are inert until this phase's challenge system writes them. New challenge code must read the `s_session_date` snapshot, never the RTC. |
| 12 | Apocalypse mode | ⬜ not started |
| 13 | Polish (LEDs, splash, etc.) | ⬜ not started |

**Open questions / parking lot:**
- **Framerate** — currently **28.3 FPS gameplay** after the
  full optimisation round landed 2026-05-11 (RGB565 + PPA
  Option A 3-op + double-buffer + narrowed lane lines +
  direct_565 line and triangle rasterizers). 3× the original
  9.5 FPS baseline. See the "Future FPS improvements" section
  for the catalogue of parked optimisations available if a
  future feature pushes the frame budget over.
- **Phase 5 sun movement** — the pipeline is ready: the SRM
  destination `block_offset_y` becomes a function of `sun_dy`;
  the FILL handles the newly-exposed sky above the sun; the
  mountain BLEND is unchanged. No per-frame re-rasterisation
  needed.
- **`esp_cache_get_alignment`** — not in the SDK header
  (`esp_cache.h` only declares `esp_cache_msync`). Hardcoded
  128 (ESP32-P4 PSRAM L2 cache line) at the call sites. If we
  ever care about other targets, swap to a runtime query.
- **Single-buffer fallback** — `bsp_display_blit` takes a buffer
  pointer, so DIY double-buffering Just Works. The BSP's own
  `num_fbs = 2` path is unused. If we ever want to drop our
  double-buffer (memory pressure, etc.), the single-buffer path
  still tears unless `bsp_display_blit` itself can be made
  synchronous — uninvestigated.


---

## Working with these docs

1. **This file** — the Current Status table above is authoritative for what
   is done and what is next.
2. **[decisions-log.md](decisions-log.md)** is append-only — read it for any
   decision made after the original plan, and append new decisions there as
   work progresses.
3. **[roadmap.md](roadmap.md)** holds the phased plan and the per-phase
   verification checklist. Pick the next not-started phase from the Current
   Status table and follow its description in the roadmap.
4. After completing a phase: flip its row in the Current Status table above,
   append the decisions to [decisions-log.md](decisions-log.md), and commit.
5. If a phase produces a non-trivial deviation from the plan, update the
   relevant section of [architecture.md](architecture.md) so the design docs
   stay current instead of letting them rot.
