# Architecture

> Project context, file layout and per-module responsibilities. Part of the [dev docs](README.md).

## Context

We are building **Race the Synth**, a Tanmatsu graceloader app at
`/home/cavac/src/tanmatsu/tanmatsu-synthracer-grace/`. The repo currently
contains the unmodified template (`main/main.c` showing input events).

The game is a clone of *Race The Sun* (Flippfly, 2013) reskinned in synthwave
aesthetic. Player pilots a craft along a procedurally generated landscape;
the sun is setting; speed boosts push it back up; obstacles kill on contact;
the player chases score and a meta-progression of unlockables.

The project must be **fully offline** — daily challenges and the daily world
seed are derived from the device RTC, not a server. The visual backdrop
reuses the launcher's existing synthwave drawing code so the game feels
visually continuous with the boot animation.

Scope confirmed with user:
- **Full 25-level metaprogression ladder** mirroring the original.
- **Hand-coded 3D-to-2D perspective projection** for obstacles and ship.
- **RTC-driven daily seed** for world layout and challenges, **with an
  optional custom-seed mode** so the player can replay the same
  level deterministically. Custom-seed runs count toward meta-progression
  the same as daily runs.
- **Simple SFX from day one** (PCM samples mixed via I2S task), no music.
- **Volume keys must change device-wide volume** (mirroring launcher/videoplayer).
- **Synthwave shapes are pre-triangulated once** at boot, redrawn per
  frame via `pax_draw_shape_triang()`, so per-frame cost is just the
  fill rasterization.
- **Z-order**: sun is drawn before mountains so it can visually sink behind
  them as `sun_seconds_left` runs out.
- **Shadow gameplay**: obstacles between the ship and the sun cast
  shadows; the ship loses speed (and the sun timer drains faster) while
  shadowed.

---

## File Layout

Following the multi-file action-game convention from `tanmatsu-placeinvaders-grace`:

```
main/
  crt0.c              # unchanged — graceloader entry glue
  main.c              # init (BSP, NVS, audio, vsync), state machine, top-level loop
  game.h / game.c     # game state struct, fixed-step update, collision, scoring
  world.h / world.c   # daily seed, region generation, obstacle/pickup pools, PRNG
  render.h / render.c # 3D projection, scene draw, HUD, title/game-over screens
  synthwave.h         # COPIED from tanmatsu-launcher/main/synthwave.{c,h}
  synthwave.c         #  ↳ provides static backdrop + scrolling grid floor
  meta.h / meta.c     # 25-level unlock table, challenges, NVS persistence
  audio.h / audio.c   # I2S mixer task, sound trigger flags, system-volume handling
  input.h / input.c   # event drain, polled steering, volume/jack handling
  sfx.h               # embedded PCM sample arrays (xxd output)
metadata/metadata.json   # update name/description/version
CMakeLists.txt           # extend APP_SOURCES with the new .c files
```

`synthwave.c` from the launcher will be **copied and refactored**. The
FreeRTOS animator task at lines 272–392 is removed (we own the frame loop).
The two pure render functions stay, but are split into composable layers
that respect the new render order:

```
synthwave_init()                  // pre-triangulate all polygons (once)
synthwave_draw_sky(fb)            // background fill
synthwave_draw_sun(fb, dy)        // 5 sun bands, vertically offset by dy
synthwave_draw_mountains(fb)      // mountain silhouette + cyan wireframe
synthwave_draw_top_grid(fb)       // top horizon line
synthwave_step(fb)                // bottom grid floor (animated)
```

Per-frame draw order in `render.c`:
sky → **sun** (with sink offset) → **mountains** (occlude sun) → mountain
wireframe → top grid line → bottom grid (`synthwave_step`) → 3D scene
(obstacles, pickups) → ship → HUD.

This is the order needed so the sun visually disappears behind the mountains
as it sets — the original launcher already used this z-order, but splitting
the function makes it explicit and lets us animate the sun.

---

## Module Responsibilities

### `synthwave.c` — pre-triangulated backdrop

- All shape vertex arrays from `tanmatsu-launcher/main/synthwave.c` (sun0..sun4,
  mountains) are kept as `static const pax_vec2f` arrays.
- `synthwave_init()` runs once at boot. For each shape it calls
  `pax_triang_concave(&indices, npts, points)` (declared at
  `include/pax_shapes.h:99`) and stashes the returned malloc'd index
  array in a static slot. Triangulation is allocated in PSRAM via
  `heap_caps_malloc(... MALLOC_CAP_SPIRAM)` since it never frees.
- `synthwave_draw_sun(fb, dy)` then iterates the 5 sun shapes calling
  `pax_draw_shape_triang(fb, color, npts, points, ntris, indices)` with
  the sun's `y` shifted by `dy`. We do **not** translate via the matrix
  stack (`pax_push_2d` would also translate clip mask); instead we keep
  a small mutable `pax_vec2f` scratch copy with `dy` baked in. Cheap —
  ≤80 floats copied per frame.
- The mountain wireframe is just `pax_simple_line()` calls — those are
  already O(1) per line, no triangulation needed; copy the
  `mountain_lines[]` table verbatim.
- `synthwave_step(fb, dz_world, cam_x)` is now a full world-space
  floor renderer, not a port of the launcher's screen-space scroller.
  Both axes of grid lines share the obstacle projection
  (FLOOR_F == RENDER_FOCAL_LEN, FLOOR_HALF_W == RENDER_HALF_W,
  floor horizon == RENDER_HORIZON_Y):
  * *Vertical lanes* — a constant-world-X ray projects to a straight
    screen-space line through the vanishing point
    `(HALF_W, horizon_y)`. Each lane is drawn from the vanishing
    point to its endpoint at `FLOOR_Z_NEAR`. kx range covers
    `±FLOOR_Z_FAR` world units of dx so lanes near the horizon are
    always present (no popping at screen edges as cam_x pans).
  * *Horizontal stripes* — anchored to fixed world-z =
    `k * FLOOR_LANE_L`. The function tracks a `cam_z` accumulator
    that advances by `dz_world` (the same step every obstacle's
    z_world decreases by) and projects each surviving stripe at
    `sy = horizon_y + FLOOR_F / (k*L - cam_z)`. Modulo filter
    `k % FLOOR_HSTRIPE_DRAW_EVERY == 0` thins the set so the
    bottom-of-screen cadence stays watchable at default speed; the
    cam_z wrap window is `LANE_L * DRAW_EVERY` so the surviving
    set is continuous across wraps.

### `world.c` — daily seed, stages, areas, content streams

> **Outdated as of 2026-05-13** — the monolithic `world.c` described
> below has been split: object spawners live in `main/objects/*`,
> area generators in `main/areas/*`, and `obstacle_t` + pool helpers
> in `main/obstacle.{c,h}`. `world.c` itself is now a slim
> orchestrator (~230 lines). The architecture description below is
> retained for the gameplay-level concepts (stages, areas, booster
> scheduling, daily seed) which are still accurate. For the
> file-layout details and the per-object callback contract, see the
> **2026-05-13 — World generation modularised** entry in the
> decisions log.


- `world_init(world_state_t*, uint32_t level_seed)` — stores the run
  seed, clears the obstacle pool, fills the side walls so they're
  visible on the first frame, and starts stage 1.
- `world_advance(world_state_t*, float dt, float ship_speed)` —
  advances the world by `dz = speed * dt`. Each frame: drift all
  active obstacles toward the camera, despawn those whose *back
  edge* has crossed the near threshold, top up the side-wall
  cursors, then tick the active area through `area_tick` and
  transition (next area / rest / next stage) when the active area
  reports done.
- Obstacles stored in a fixed pool of `WORLD_OBSTACLE_POOL_SIZE`
  (= 128) entries of
  `{ kind, x, z, half_w, half_d, height, four-colour palette,
  active }`. The `obstacle_kind_t` enum tags each entry as `CUBE`,
  `WALL`, one of the pickup variants, or `RAMP`. Collision (and
  later render) dispatches on it — adding a new obstacle type
  means one enum value + a case in each switch. The renderer
  treats every entry uniformly (today: 3D cube draw for CUBE +
  WALL; pickups/ramps will get their own draw functions when
  those phases land).
- Side walls are stored as regular obstacles so a single AABB
  collision pass covers both the dynamic stream and the track
  edges. Segment length = `FLOOR_LANE_L * FLOOR_HSTRIPE_DRAW_EVERY`
  (= 3) so each segment runs from one drawn floor stripe to the
  next; segment centres are offset by half a stride so the joins
  land on stripes rather than between them. Two per-side
  `*_wall_far_z` cursors track where the next far-end segment
  should spawn; each frame the cursor slides forward by `dz` and
  any time it dips inside `WORLD_Z_FAR_SPAWN` we drop a fresh
  segment in.

- **Stage / area state machine.** The world is split into
  fixed-length stages of `WORLD_STAGE_LENGTH_Z` (= 720 u, ~60 s
  at base speed) followed by `WORLD_REST_LENGTH_Z` (= 120 u, ~10
  s) rest areas. Inside a stage the world picks **obstacle
  areas** uniformly at random from the types whose
  `min_stage <= current stage`; the active area's generator
  populates the world until the area's length budget is
  consumed, then the next area is picked. When the stage budget
  is exhausted *after* the current area finishes, the rest area
  inserts and the stage counter ticks over. Areas always run to
  completion — stages may overshoot the budget slightly so the
  player never sees a clipped area.

- **Per-stage PRNG.** `stage_prng = mix_stage_seed(level_seed,
  stage)` is re-derived at every stage transition (xorshift
  rounds over the golden-ratio-mixed combination). A run-seed
  reproduces every stage's content identically. The `level_seed`
  field on `world_state_t` is preserved across stage rollovers;
  `stage_prng` is the working state consumed by area picks,
  obstacle x positions, gate counts, etc.

- **Area types** (all `min_stage = 1` today; new types add an
  entry to `pick_area_type` gated by their `min_stage`):
  * `AREA_TYPE_PIXEL_FIELD` — the original small-magenta cube
    stream. Length budget 116..232 u (2..4 screens). Spawn
    interval 12-22 u at stage 1; scaled by `stage_interval_scale`
    (-5%/stage down to a 0.5× floor reached at stage 10).
  * `AREA_TYPE_BIG_BLOCKS` — 2× lateral / 2× depth cubes, grey
    palette, same height. Sparser cadence (20-35 u base, same
    per-stage scale). Tagged `OBSTACLE_KIND_CUBE`; the renderer
    and collision treat them identically to pixel cubes — only
    dimensions and palette differ.
  * `AREA_TYPE_GATEWAYS` — wall slabs spanning the playfield
    width with a single ship-sized opening. Each gate is two
    `OBSTACLE_KIND_CUBE` slabs flanking the gap (amber palette);
    head-on into a slab is fatal exactly like striking a pixel
    cube. Gap width `lerp_by_stage(stage, 3*ship_w, 1.5*ship_w)`,
    clamped past stage 10. Inter-gate / lead-in / trailing pad
    is a fixed 50 u for all stages (sized so back-to-back
    hard-left ↔ hard-right gaps remain reachable at cruise
    speed). Gate count uniform 1..5.
    Layout: `[settle][pad][gate][pad][gate]...[gate][pad]` —
    total length `settle + (n+1)*pad + n*thick`. The `settle =
    WORLD_Z_FAR_SPAWN` (= 100 u) prefix guarantees any
    previous-area obstacle has crossed the camera before the
    first gate's alignment lead-in begins counting.
  * `AREA_TYPE_REST` — internal-only, not pickable by
    `pick_area_type`; inserted only on stage rollover. Today
    empty; Phase 5 will sprinkle bonus pickups here on a
    separate cadence.

- **Seed sources** (priority order, picked at title screen):
  1. **Daily seed** (default, **landed**): `seed = year*10000 +
     month*100 + day` from `time(NULL)` / `localtime_r()`,
     captured once at app boot in `derive_daily_seed()`. If the
     year is < 2024 the RTC is unset — the fallback is the fixed
     constant `1`, so the game stays playable (deterministically)
     until the clock is set. No clock-rollback anti-cheat: this
     is an offline, single-player, open-source game with no
     online leaderboard, so replaying an old seed harms nobody.
  2. **Custom seed** (**TODO**): player enters a seed on the
     title screen via a "Custom Seed…" menu item. Used for
     replaying the same world repeatedly. Custom-seed runs
     **do** award meta-progression (challenge points, level-up,
     unlocks) exactly like daily-seed runs — see the 2026-05-15
     decisions-log entry. There is no functional difference
     between a daily run and a seeded run other than which seed
     drives world generation; the highscore *for a specific
     seed* may still be tracked in a small per-seed best ring
     for the title screen.
- The seed mode is set by `world_init(seed)`. No `is_custom`
  flag — every run feeds meta-progression the same way.

- **Pickup pool** — Phase 9 will extend the existing
  obstacle pool with `OBSTACLE_KIND_PICKUP_*` entries (stubs
  already in the enum) rather than maintaining a parallel
  pool. Same dimensions / colour fields, kind-dispatched
  collision response.

- **Regions** (Phase 10) — **dissolved 2026-05-13.** The
  original plan was a discrete 7-region table cut-in on top
  of stages, with per-region palette / density / area-weight
  mutators. In practice the stage + area-picker machinery
  evolved to cover this: new area types (bridges,
  dynamic_passage, dynamic_gateway) land with their own
  palettes, layouts, and stage-range gates, and the picker
  picks per-stage from whatever's applicable. We'll keep
  growing the area library and tuning stage gates as the rest
  of the phases land, rather than carve out a separate region
  layer.

### `game.c` — gameplay update

- `game_state_t game` holds run-time state: `ship_x_world`,
  `ship_speed_z`, `bank` (-1..+1 signed banking factor), `cam_x`,
  plus (in later phases) sun-time-remaining, score, multiplier,
  pickup inventory, region index, region-progress flags.
- Fixed timestep: 1/60 s. We compute `dt` from `esp_timer_get_time()` and
  step the simulation in 16.67 ms chunks; rendering interpolates only if
  surplus time.
- **Banking-driven steering**: each frame `bank` moves toward
  `target = (float)steer` at `BANK_ACTIVE_RATE` while any
  direction is held and at the slower `BANK_PASSIVE_RATE` when
  the stick is released. Lateral velocity is purely
  `bank * SHIP_TURN_RATE` — a smooth bank ramp gives a smooth
  turn ramp, no separate friction model needed. The asymmetric
  active/passive rates are what makes "release the key" feel
  different from "press the opposite key": releasing drifts back
  slowly, reversing snaps over at the full active rate. The
  rendered roll = `bank * MAX_BANK_RAD` so the visible bank and
  the actual turn rate are one and the same parameter.
- Pickups (Phase 9+) activated via spacebar / button (queued events).
- Collision: `game_collide` does AABB in 3D world space against
  every active entry of the obstacle pool (walls, dynamic
  obstacles, future pickups all share the same pool).
  Classification dispatches on `obstacle_t.kind`:
  * `OBSTACLE_KIND_WALL` — scrape only. Set
    `scrape_left`/`scrape_right` and push the ship out along x
    by `x_pen` so it physically can't penetrate the wall.
  * `OBSTACLE_KIND_CUBE` — head-on iff the obstacle's near face
    was still ahead of the ship's front face at the *start* of
    this frame (i.e. it entered the ship's z range from ahead
    during the frame). Returns true; caller flips the app state
    to `GAME_OVER`. If the obstacle was already overlapping or
    past the ship last frame, the contact is a trailing scrape.
    The check uses the obstacle's reconstructed previous z
    position (`z_world + speed*dt`) so it's robust to per-frame
    z motion exceeding the overlap window — see the
    2026-05-11 swept-z entry in the decisions log.
  * `OBSTACLE_KIND_PICKUP_*` / `OBSTACLE_KIND_RAMP` — stubs
    that `continue` for now. Phase 5 / 6 / 9 / future fill
    these in.
  `game_after_collide` reads the resolved scrape flags and ramps
  `ship_speed_z` toward the scrape-floor (≈ 0.55 × base) at
  `SCRAPE_DECEL` while scraping, or back up toward base speed at
  the slower `SPEED_RECOVERY` when contact ends — both
  transitions are continuous. Phase 9 will add a pickup
  proximity test (or extend the pickup `case` clauses above) that
  respects the magnet upgrade range.
- **Sun mechanic & shadow**: `sun_seconds_left` ticks down at 1.0/s in
  light, 1.5/s in shadow. Each tick `game_step()` calls
  `is_ship_in_shadow(world, ship)`:
  - For every active obstacle, check three conditions:
    - `obs.z_world > ship.z_world` (obstacle is between ship and sun, i.e.
      ahead of the camera)
    - `|obs.x_world - ship.x_world| < (obs.half_w + ship.half_w +
      shadow_padding)` (lateral overlap with a small fudge — taller
      obstacles cast wider shadows, so `shadow_padding` scales with
      `obs.height`)
    - `obs.height > shadow_min_height` (a low pyramid-pickup-sized object
      doesn't block the sun)
  - If any obstacle satisfies all three, the ship is shadowed for this
    tick. While shadowed, `ship.speed *= 0.985` per tick (compounds to
    a noticeable but recoverable slowdown over ~1 second of shadow).
  - When the ship leaves shadow, `ship.speed` re-accelerates back toward
    `base_speed` at a fixed `accel_per_sec`.
  - Visual feedback: `render.c` reads the shadow flag and tints the ship
    sprite ~30% darker plus dims a small region of the floor grid behind
    the ship for that frame.
- Crash: instant death unless a Shield is consumed. Boost adds N seconds to
  `sun_seconds_left` AND clamps `ship.speed` back to `base_speed` so it
  doubles as an "escape from a shadow stall" tool. Reaching `<= 0` ends
  the run.
- Multiplier (Phase 6 — design refreshed 2026-05-13 /
  2026-05-14). `game_state_t.pickups_tri` is **monotonic per
  run**: it counts every Tri collected and never resets
  mid-run. The HUD's 0/5 fill-and-reset display is just
  `pickups_tri % 5` — cosmetic, no separate state. On every
  Tri pickup: `pickups_tri++`; if it's now a non-zero
  multiple of 5, `multiplier++`. The "(pickups_tri - 1) % 5"
  slot index drives both the HUD's lit-triangle position
  and the ascending pentatonic plink SFX. Crash drops
  `multiplier` by 5; floor is `1` until Phase 11
  metaprogression raises it (lv6→2, lv12→3, lv23→4,
  lv24→max). Tri counters and HUD progress are *not* rewound
  by a crash. **Scoring** has three streams, all multiplied
  by the same `multiplier`: (1) per-frame distance
  accumulation `score += dz × multiplier ×
  GAME_SCORE_DISTANCE_FACTOR` (factor = 0.1) in
  `game_after_collide` — the always-on income stream from the
  original RTS model, scaled down so pickup bonuses stay
  perceptible; (2) Tri pickup bonus
  `GAME_SCORE_TRI × multiplier` in `game_collide`'s
  Tri-collect branch; (3) booster pickup bonus
  `GAME_SCORE_BOOSTER × multiplier` in the same place.
  Persistence is free — `last_run.pickups_tri = pickups_tri`
  at run end and the existing `run_stats_merge_into_all_time`
  sums it into the all-time total.

### `render.c` — projection, scene submission, shadows

- Frame entry, **explicit per-frame layer order** (matters for occlusion):
  1. `synthwave_draw_sky(fb)` — purple background fill
  2. `synthwave_draw_sun(fb, sun_dy)` — pre-triangulated sun bands, shifted
     downward as the sun-timer drains. Drawn **before** mountains so the
     mountain silhouette occludes the lower half of the sun naturally as
     it sinks. `sun_dy = (1.0 - sun_seconds_left/SUN_MAX) * SINK_RANGE`.
  3. `synthwave_draw_mountains(fb)` — pre-triangulated mountain polygon
  4. `synthwave_draw_wireframe(fb)` — cyan mountain lines
  5. `synthwave_draw_top_grid(fb)` — single magenta horizon line
  6. `synthwave_step(fb)` — animated grid floor; scroll speed modulated by
     ship speed.
  7. `render_shadows(fb)` — flat floor-shadow trapezoids (2D decals on the
     floor — *not* depth-buffered).
  8. **3D scene** — `scene_begin` → `render_submit_obstacles` →
     `game_submit_ship` → `scene_flush`. Visibility is a per-pixel
     z-buffer (see `scene.c`), not a painter's sort; obstacles and the
     ship are all geometry in the same depth-tested pass.
  9. HUD (score, multiplier, stage, pickup inventory, optional volume bar)
- 3D projection: pinhole camera at `(cam_x, cam_y, 0)` looking down +Z.
  World coordinates: x = lateral, y = vertical, z = depth (positive =
  forward). `render_project` does `sx = HALF_W + f*(x-cam_x)/z`,
  `sy = HORIZON_Y - f*(y-cam_y)/z` for the 2D floor-shadow quads;
  `scene.c` carries its own copy of the same projection that also yields
  1/z depth. The camera is a render-module global — `render_set_camera`
  / `render_camera`, set once per frame.
- **Geometry-emitter model.** `render_submit_obstacles` walks the
  obstacle pool in pool order (no sort) and dispatches each obstacle to
  an emitter: its per-object `obstacle_t.emit` callback if set, else the
  default cube / pyramid / icosahedron emitter chosen by `kind`. An
  emitter computes geometry in **world space** and hands it to
  `scene_tri` / `scene_line` — it never projects or rasterizes itself.
  Per-face camera-side culls are kept only as a speed optimisation.
- **Adding a custom-shaped object:** the object module writes a
  `static void <obj>_emit(obstacle_t const* o)` that submits world-space
  triangles + wireframe edges via `scene_tri`/`scene_line` (reading
  `render_camera()` for any back-face culling), and assigns it to
  `o->emit` at spawn. No projection, no draw order, no rasterization in
  the object — the z-buffer resolves visibility. `objects/ramp.c`,
  `objects/jump_booster.c` and `objects/flipping_cube.c` are the worked
  examples; the bridge span needs no emitter at all (the default
  `y_base`-aware cube emitter renders it).
- The ship is emitted by `game_submit_ship` (see `game.c`) into the same
  scene, so it is depth-tested against obstacles like any other object.
- HUD: top-right score + multiplier; top-left region indicator and
  challenge progress; bottom-left pickup inventory icons. Use
  `pax_font_saira_condensed` (faster) and `pax_clip()` so HUD doesn't fight
  the synthwave overlay.
- Title/game-over screens: synthwave backdrop + animator-style scrolling
  grid + centered text, no scene rendering.
- **Title screen menu** (selectable with up/down + space):
  - "Play" — start a daily-seed run
  - "Custom Seed…" — opens a small input dialog. The Tanmatsu has a real
    QWERTY keyboard, so we read `INPUT_EVENT_TYPE_KEYBOARD` ASCII digits
    into a `char buf[11]` (max 10 digits → fits a `uint32_t`), Backspace
    to edit, Enter to confirm, Esc to cancel. Stored in NVS as
    `last_custom_seed` for default-fill on next visit.
  - "Stats" — show level, points to next, all-time highscore, today's
    challenges + their progress.
  - "Exit" — `bsp_device_restart_to_launcher()` (also bound to F1).

### `scene.c` — depth-buffered 3D rasterizer

The per-pixel z-buffer that replaced the per-object painter's algorithm
(landed 2026-05-19; see that date's decisions-log entries for the full
rationale). Not in the original plan.

- Owns three PSRAM buffers, allocated once by `scene_init()`: a
  `uint16_t` depth buffer (scaled 1/z; larger = nearer), an 8-bit
  per-pixel **frame-stamp** plane, and a deferred wireframe-edge buffer.
- `scene_begin(fb)` — binds the framebuffer and advances the frame
  stamp. **No depth memset:** a depth value counts only if its stamp
  equals the current frame, so a stale pixel reads as infinitely far.
  This is what makes the frame start cost a counter increment instead
  of a 768 KB clear.
- `scene_tri(world ×3, argb)` — projects (per-vertex near-clip clamp),
  then rasterizes **immediately** with a per-pixel 1/z depth test +
  write. Triangle submission order is irrelevant, so there is no sort
  and no triangle buffer.
- `scene_line(world ×2, argb)` — projects and **defers** the edge into
  the edge buffer.
- `scene_flush()` — rasterizes every deferred edge last, depth-tested
  with a ×1.02 nearer bias (an edge beats the coplanar face it outlines
  but still loses to genuinely nearer geometry) and **no** depth write.
- Visibility is per-pixel correct: stacked, straddling and
  interpenetrating geometry all resolve with no draw-order reasoning.
  Objects just emit world-space geometry (see the `render.c`
  geometry-emitter model); `scene.c` does projection, depth and
  rasterization centrally.
- Cost: `obs` ≈ 21 ms (vs ≈ 6 ms for the old painter's renderer) — the
  price of the per-pixel depth test. See the 2026-05-19 optimisation
  entry; further `obs` gains are diminishing returns.

### `save.c` — persistence + slot management + stats (replaces planned `meta.c` persistence)

Persistence lives in plain NBT files under `/int/synthracer/`, one
per slot. The format is copied verbatim from
`tanmatsu-paperclips-grace/main/game_nbt.{c,h}` — only the magic
(`SYNT`) and the structure of the payload differ.

**Files in `main/`:**
- `nbt.{c,h}` — generic NBT reader/writer, header-for-header
  identical to paperclips with `NBT_MAGIC_0..3 = 'S','Y','N','T'`.
- `save.{c,h}` — slot API (`save_init`, `save_load_slot`,
  `save_write_slot`, `save_slot_exists`, `save_slot_peek`,
  `save_commit_run_end`).
- `save_nbt.c` — `save_write_state` / `save_read_state` that
  serialize the `save_data_t` struct to / from an open NBT
  reader / writer. Skipping unknown tags lets us add new
  fields without breaking older saves; missing fields keep
  their `save_init_defaults()` values.

**Tools (in `tools/`):**
- `savetool.pl` — copied from paperclips, magic patched to
  `SYNT`. `./tools/savetool.pl decompile save0.bin out.txt`
  produces an indented text dump; `./tools/savetool.pl
  compile in.txt save0.bin` reconstructs the binary.
  Round-trip-stable, so we can construct test scenarios by
  hand (e.g. "slot with stage_best=12 and the multiplier
  unlock") and push them to the device via `mpremote` or
  similar.

**Slots:**
- `SAVE_SLOT_COUNT = 3`. No autosave slot. Slot 0/1/2 are
  independent profiles.
- `save_data_t s_save` is the in-memory mirror of the active
  slot's file contents. The active-slot index is sticky for
  the app lifetime.
- On boot: enumerate the three files, show
  `STATE_SLOT_SELECT` with per-slot summary lines (last
  played date, best score, runs played; or `[new]` if the
  file is absent). The user picks a slot; we
  `save_load_slot(idx)` (or initialize defaults if the file
  doesn't exist yet), set `s_active_slot = idx`, and
  transition to `STATE_MENU`.
- On run end: `save_commit_run_end(reason, &game)` updates
  the in-memory `s_save.stats` (last_* fields = current run,
  best_* fields = max(current, previous best), all-time
  totals += this-run delta) and immediately writes the file
  via `save_write_slot(s_active_slot)`. Synchronous — the
  player is on the GAME OVER screen anyway.

**`save_data_t` (in memory; serialised compound-for-compound):**

```c
typedef struct {
    // peek (written first so save_slot_peek can read this fast)
    int64_t last_played_unix;       // time(NULL) of last save, 0 if never
    int64_t score_best;              // copied here from stats for fast peek
    int32_t stage_best;
    int32_t runs_total;

    // stats — last-run snapshots + all-time accumulators
    struct {
        int64_t score_last, score_best;
        double  distance_last, distance_total;
        int32_t stage_last, stage_best;
        int32_t multiplier_last_max, multiplier_best;
        int32_t run_end_reason;     // 0=none,1=crash,2=stall,3=sunset,4=quit
        int32_t runs_total, runs_crashed, runs_stalled, runs_sunset, runs_quit;
        double  play_time_total_s;
    } stats;

    // meta — player progression + flags (each unlock is its own bool)
    struct {
        int32_t level;               // 1..25, default 1
        int32_t points;              // toward next level
        // 25 explicit booleans — INT32 0/1 each
        int32_t unlock_speed_boost;  // lv2
        int32_t unlock_multiplier;   // lv3
        int32_t unlock_jump;         // lv4
        int32_t unlock_magnet;       // lv5
        int32_t unlock_starting_mult_2x;     // lv6
        int32_t unlock_portal_easier_world;  // lv7
        int32_t unlock_double_jump;          // lv8
        int32_t unlock_shield;               // lv9
        int32_t unlock_shield_attachment;    // lv10
        int32_t unlock_apocalypse;           // lv11
        int32_t unlock_starting_mult_3x;     // lv12
        int32_t unlock_attach_slot2;         // lv13
        int32_t unlock_left_wing_decal;      // lv14
        int32_t unlock_double_portal;        // lv15
        int32_t unlock_checkpoint;           // lv16
        int32_t unlock_power_turning1;       // lv17
        int32_t unlock_power_turning2;       // lv18
        int32_t unlock_triple_jump;          // lv19
        int32_t unlock_checkpoint2;          // lv20
        int32_t unlock_enhanced_magnet;      // lv21
        int32_t unlock_right_wing_decal;     // lv22
        int32_t unlock_starting_mult_4x;     // lv23
        int32_t unlock_starting_mult_max;    // lv24
        int32_t unlock_labyrinth;            // lv25
        int32_t attach1, attach2;            // equipped attachment IDs (0 = none)
        int64_t last_custom_seed;            // most recently entered seed
        int64_t last_seen_date;              // yyyymmdd; day-rollover detection
    } meta;

    // daily — today's challenge state (resets when last_seen_date rolls
    // forward; each "done" is its own boolean)
    struct {
        int32_t daily_done_1pt;
        int32_t daily_done_2pt;
        int32_t daily_done_3pt;
        // challenge targets / progress arrive with Phase 11.
    } daily;
} save_data_t;
```

`save_init_defaults(&s)` zeros the struct and sets `meta.level = 1`.

**API:**
- `void save_init(void);` — `mkdir /int/synthracer` if absent.
- `int  save_slot_exists(int slot);` — `stat(path)`.
- `int  save_slot_peek(int slot, save_peek_info_t* out);` —
  reads just the `peek` compound from disk; for slot-select
  summary display.
- `int  save_load_slot(int slot, save_data_t* out);` —
  initializes defaults, then reads tags, ignoring unknowns.
  Returns 0 on success, -1 if the file is missing / corrupt.
- `int  save_write_slot(int slot, save_data_t const* s);` —
  serialises the full struct to the slot file.
- `void save_commit_run_end(save_data_t* s, int reason,
  game_state_t const* g);` — updates stats from the just-ended
  run, then `save_write_slot()`. The single entry point for
  end-of-run persistence so the call site in main.c stays one
  line.

**Day-rollover (planned, not yet implemented):** `meta.last_seen_date`
is purely a record of the last calendar day the player launched the
game. On boot, if the current RTC date differs from `last_seen_date`,
reset the `daily_done_*` booleans and store today. No clock-rollback
anti-cheat — this is an offline single-player open-source game with no
online leaderboard, so the player is free to replay any seed they like
(including by setting the clock). Rolling the clock back simply gives
the player that day's world again; nothing is gated on it.

**Custom seed:** `last_custom_seed` is updated whenever the
user starts a seeded run; the seed-input screen prefills its
buffer from this field. Phase 11 will add the per-seed best
ring; today only the most-recent value is persisted.

### Audio subsystem — mixer + procedural music + procedural SFX

Replaces the original single-`audio.c` plan with a layered design.
See the **2026-05-13 audio subsystem** decisions-log entry above
for the full rationale; the bullet list below is the short
reference.

- **`main/audio_mixer.{c,h}`** — owns I2S, runs the mixer task,
  enforces the 22050 Hz / s16 / stereo pipeline format. One
  music slot (gain ≈ 30%), N=8 SFX voice slots, idle-drain
  power-down (mute amp + disable I2S after ~46 ms silence),
  explicit `audio_mixer_shutdown()` for app exit.
- **`main/audio_source.h`** — `music_source_t` trait struct
  (render / on_seed / shutdown). Mixer is backend-agnostic so
  procedural now / modplayer or MP3 / MIDI later all plug in
  without touching the mixer.
- **`main/audio_dsp.{c,h}`** — shared primitives used by both
  music and SFX: oscillators (sine via lookup, saw, square,
  triangle, noise), ADSR, biquad filters, soft-clip.
- **`main/music/music_procedural.{c,h}`** — initial music source.
  Algorithmic synthwave: chord-progression bank, bass / arp /
  pad / drum layers, structural mutation every ~16 bars,
  seeded by a *separate* `music_prng` split from the run seed
  (so music vs. world generation never perturb each other).
- **`main/sfx/sfx_*.{c,h}`** — five effects, one `.c/.h` per
  effect: `sfx_engine_hum` (persistent, set_pitch(ship_speed_z)),
  `sfx_pickup_ding` (one-shot), `sfx_crash` (one-shot),
  `sfx_scrape` (persistent + intensity), `sfx_cube_bump`
  (one-shot). Procedurally generated — no PCM data on disk.
  Each exposes its own typed API; internally each registers
  one `sfx_voice_t` with the mixer.
- **`main/audio_settings.{c,h}`** — two u8 NVS flags
  (`audio_music_on`, `audio_sfx_on`, default 1) in the
  existing `synthracer` namespace. Master gates inside the
  mixer skip music / SFX render passes entirely when disabled.
- **Music life cycle** — installed on `start_run()`, present
  through `STATE_PAUSED` (pause is "still in the run"),
  cleared on `STATE_GAME_OVER` entry. Idle-drain handles the
  speaker mute within ~50 ms.
- **System volume** continues to mirror
  `tanmatsu-launcher/main/global_event_handler.c`: VOLUME_UP /
  VOLUME_DOWN keys read/write the `"system"` NVS namespace's
  `speaker.volume` / `hp.volume` u8s, audio-jack-insert
  re-applies the amplifier + volume for the new output. This
  is *separate* from the per-app `audio_music_on` /
  `audio_sfx_on` toggles in `audio_settings`.

### `input.c` — event drain + polled steering

- `input_drain()` non-blocking dequeue of all queued events; updates a
  small `held_keys` struct.
- Steering: poll `BSP_INPUT_NAVIGATION_KEY_LEFT` / `_RIGHT` each tick via
  `bsp_input_read_navigation_key()` — gives smooth analog-feeling steering
  without depending on event repeat rate.
- Action button: spacebar (scancode `0x39`) for use-pickup; `F1` for
  restart-to-launcher; `F2`/`F3` for backlight (matches template).
- Volume keys handled inside the drain (delegated to `audio.c`).

### `main.c` — top-level

Replace the current event-printing demo with:

1. NVS init, BSP init (RGB565 framebuffer since 2026-05-11 — see
   the "Performance baseline" decisions-log entry; num_fbs=1 — same
   as template).
2. `pax_buf_init` + orientation (template code already handles this).
3. Tearing-effect / vsync semaphore setup
   (`bsp_display_set_tearing_effect_mode(BSP_DISPLAY_TE_V_BLANKING)` and
   `bsp_display_get_tearing_effect_semaphore`), like floppybird.
4. Audio init: `bsp_audio_initialize(16000); bsp_audio_get_i2s_handle(...);
   bsp_audio_set_amplifier(!hp); bsp_audio_set_volume(load_volume());`
   then spawn the audio mixer task.
5. `meta_load()`, choose daily seed from RTC, `world_init(seed)`,
   `meta_reroll_for_date(seed)`.
6. State machine: `STATE_TITLE` → `STATE_PLAYING` → `STATE_GAME_OVER` →
   back to `STATE_TITLE`. Same pattern as floppybird (`main.c:559–648`).
7. Per frame: drain input → fixed-step update(s) → render → blit → take
   vsync semaphore (timeout 50 ms fallback).

---

