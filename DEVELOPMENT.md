# Race the Synth — Implementation Plan & Tracking

> **This file is the canonical, cross-session tracking document for the
> project.** It contains the original-game research, every design decision
> already made, the phased plan, and the current status. Update it as work
> progresses so a fresh session can resume without re-researching anything.
>
> Once plan mode is exited, this file should be mirrored into the repo as
> `tanmatsu-synthracer-grace/DEVELOPMENT.md` so it travels with the code.

## Current Status

**Phase: Phase 3 complete; Phase 4 next**

| Phase | Description | State |
|-------|-------------|-------|
| 0 | Research, design, plan | ✅ done — see this document |
| 1 | Skeleton + synthwave backdrop | ✅ runs on-device; sun arch fixed, Hershey font, F-key icon hint, horizon lifted, grid floor extended with extra perspective lines on each side |
| 2 | Ship + steering | ✅ runs on-device; ship is a placeholder delta-wing (sun-yellow + grid-magenta), proper graphics deferred to Phase 13 |
| 3 | 3D projection + obstacles | ✅ runs on-device; floor stripes + lanes + obstacles share one pinhole projection; obstacles render as 3D cubes with per-face culling and near-plane clipping; continuous side walls along the track edges, all stored in the same obstacle pool ready for collision in Phase 4 |
| 4 | Collision + game over | ⬜ not started |
| 5 | Sun timer + shadow + boost | ⬜ not started |
| 6 | Tris + multiplier | ⬜ not started |
| 7 | Audio + volume keys | ⬜ not started |
| 8 | Daily + custom seed + persistence | ⬜ not started (MVP complete after this phase) |
| 9 | Pickups & attachments | ⬜ not started |
| 10 | Regions | ⬜ not started |
| 11 | Meta-progression UI | ⬜ not started |
| 12 | Apocalypse mode | ⬜ not started |
| 13 | Polish (LEDs, splash, etc.) | ⬜ not started |

**Open questions / parking lot:**
- **Framerate** — landed the backdrop cache (see decisions log
  2026-05-07). On-device measurement still pending. If still slow,
  next candidates: Hershey direct text per-pixel cost, the wide grid
  perspective line set (~78 lines per frame), and the per-frame
  full-FB memcpy itself (1.15 MB at 60 fps = 69 MB/s of PSRAM
  bandwidth — measurable but should be fine).
- **PPA / 2D-DMA / double-buffer** — checked: PPA HAL/LL headers are
  present but no high-level `driver/ppa.h` symbols in fakelib;
  `dma2d_*` symbols ARE in fakelib (PPA's transport, usable as a
  hardware memcpy with non-trivial setup); `bsp_display_blit` takes
  the user's buffer pointer with no swap API exposed. **Decision**:
  punt on hardware accel for now. If the plain-memcpy backdrop ever
  becomes the bottleneck, add high-level PPA bindings to graceloader
  before touching the app — keeps the app code simple.
- **Phase 5 sun re-cache** — when the sunset mechanic lands, the
  cached sun position becomes stale as `sun_dy` changes. Plan: track
  the dy used to render the cache, re-render only when the new dy
  differs by more than ~2 px. Most frames stay on the memcpy path.

**Conventions / decisions log** (append-only as new decisions are made):
- 2026-05-07 — Project bootstrapped from `tanmatsu-template-grace`.
- 2026-05-07 — Volume keys must update the launcher-shared `"system"` NVS
  namespace, not a private one.
- 2026-05-07 — Custom-seed runs do not award meta-progression.
- 2026-05-07 — Synthwave shapes are pre-triangulated once at boot.
- 2026-05-07 — Sun is drawn before mountains so it can sink behind them.
- 2026-05-07 — Obstacles cast shadows that slow the ship.
- 2026-05-07 — Game name finalized as **"Race the Synth"**.
- 2026-05-07 — Input mapping locked in (see Input Mapping section):
  polled D-pad/WASD steering, single pickup-use button (Space /
  Gamepad-A), F1 = exit, volume keys are system-wide, barrel-roll
  detection deferred to Phase 13.
- 2026-05-07 — Added ESC (left) and Backspace (right) as ergonomic
  thumb-rest steering keys, **modal**: only active during
  `STATE_PLAYING`. In menus / seed-entry / pause they retain their
  conventional cancel/edit roles. Pause moved from ESC to **F4** to
  free ESC for steering.
- 2026-05-07 — Build command is `make clean build` — single command
  from project root. **Never** source `$IDF_PATH/export.sh` or other
  IDF export scripts directly; the project Makefile handles its own
  env plumbing.
- 2026-05-07 — Phase 1 implementation landed: synthwave refactored into
  layered draw functions in `main/synthwave.{c,h}`; main.c stripped to
  init + vsync-paced main loop drawing the static backdrop + scrolling
  grid + "Press F1 to exit" prompt. App slug set to
  `at.cavac.racethesynth` in Makefile.
- 2026-05-07 — Sun arch fix: `sun0_pts` had a 34th trailing point
  `{300, 168.72115}` that the launcher's source ignored via
  `count = 33` in `pax_draw_shape`. Our refactor used `sizeof()` so it
  passed 34, which made `pax_triang_concave` produce a self-intersecting
  triangulation and the dome silently disappeared. Dropped the trailing
  point in `synthwave.c`.
- 2026-05-07 — Adopted Hershey vector font from
  `tanmatsu-paperclips-grace` (`hershey.h`, `hershey_font.h`,
  `hershey_font_direct.h`, `rendertext.{c,h}`) for all in-game text
  rendering. The "direct" path writes pixels straight to the pax_buf_t,
  bypassing pax's text rasterizer — faster than `pax_draw_text` per
  frame.
- 2026-05-07 — Added `main/icons.{c,h}` for loading function-key PNG
  glyphs from `/int/icons/` (mirroring `tanmatsu-camera/main/icons.c`
  but using `pax_draw_image()` for alpha-correct compositing since our
  framebuffer is pax-managed). The Tanmatsu's keyboard prints symbols
  rather than "F1"/"F2"/etc text, so HUD prompts use the icons with a
  text fallback if the launcher's icon set isn't installed.
- 2026-05-07 — Phase 2 implementation landed: `input.{c,h}` (modal
  polled steering for D-pad / WASD / Esc-Backspace, queued events for
  pickup-button and F1-exit), `game.{c,h}` (ship state + physics,
  exponential-decay friction, screen-space delta-wing draw in
  sun-yellow + grid-magenta). Main loop now uses `esp_timer_get_time()`
  for dt and clamps it to 0.1 s to survive long stalls.
- 2026-05-07 — Synthwave horizon lifted by 98 px
  (`MOUNTAIN_LIFT_PX = SUN_LIFT_PX = GRID_LIFT_PX = 98.0f` in
  `synthwave.c`) so the highest mountain peak is roughly at the
  pre-lift sun-top altitude, with sun and ground floor lifted by the
  same amount so the relative geometry is preserved.
- 2026-05-07 — Grid floor: vertical perspective lines now generated
  over a parametric `k` range derived from the new horizon, instead
  of the launcher's hard-coded 17. Same generating function ⇒ same
  per-line slopes ⇒ angle preserved. This fills the wedges of empty
  floor on the left/right that appeared after the lift compressed
  the lines toward the vanishing point. Horizontal scanline count
  is also derived from the new floor height. Setting
  `GRID_LIFT_PX = 0` reproduces the launcher's original 17-line look.
- 2026-05-07 — F1 exit hint anchored at the top-left margin (12 px
  in) with the F1 icon followed by the "to exit" Hershey text.
- 2026-05-07 — Backdrop cache: a second `pax_buf_t` (`backdrop_cache`)
  is allocated in PSRAM at boot, same dimensions/format/orientation/
  endianness as the main fb. The static synthwave layers (sky, sun,
  mountains, wireframe, top horizon) render into it once. Each frame
  the main fb is initialized via `memcpy(pax_buf_get_pixels(&fb),
  backdrop_pixels, size)` instead of re-running the triangulated
  fills + ~200 wireframe lines. The cast-away-const on
  `pax_buf_get_pixels` is intentional — pax's buffer is logically
  mutable, the const just communicates "don't muck with this via
  arbitrary writes". The memcpy is ~1.15 MB per frame on RGB888.
- 2026-05-07 — Phase 3 implementation landed: `world.{c,h}` (fixed
  pool of 64 obstacles, xorshift32 PRNG, randomized z-spawn cadence
  in world units so spawn density is speed-independent) and
  `render.{c,h}` (pinhole projection at horizon y=256, painter's
  algorithm via insertion-sorted z-descending index list, front-face
  triangle pair + cyan outline per obstacle). `synthwave_step` now
  takes a float `scroll_pixels`, accumulates internally, and the main
  loop drives it from `scroll_px_per_world_unit *
  game.ship_speed_z * dt` — floor scroll and obstacle approach now
  share the same forward-speed source. World seed is currently
  derived from `esp_timer_get_time()`; Phase 8 will swap that for the
  RTC-derived daily seed.
- 2026-05-07 — Phase 3 playfield rework after first on-device
  feedback (too many obstacles, too fast, faster than ground, no
  off-screen movement):
  * Cut `SHIP_BASE_SPEED_Z` from 30 → 12 u/s (8 s from spawn at
    z=100 to ship plane).
  * Cut spawn density: spawn cadence 6-14 → 12-22 z-units (≈0.5-1
    obstacle/sec at the camera).
  * Bumped `scroll_px_per_world_unit` from 2 → 4.5 so the floor's
    visual speed matches the ground-point approach speed at z≈10
    (the depth where most obstacle motion is read).
  * **Ship moved from screen-px to world coords**: `ship_x_world ∈
    [-5, +5]`, accel/maxvx re-tuned in world units. Game state
    gains `cam_x` which locks to `ship_x_world` so the ship stays
    centred on screen and the world pans around it. `render_project`
    and `render_obstacles` take `cam_x`. `game_draw_ship` now
    projects through `render_project` (placeholder triangles still
    a fixed pixel size; phase 13 polish will perspective-scale them).
  * **Floor lanes redrawn in world space**: replaced the launcher's
    stylized fan with vertical lane lines anchored at world_X = k *
    1.0 for integer k, projected from `FLOOR_Z_TOP=10` (just below
    horizon) to `FLOOR_Z_NEAR=2` (bottom). cam_x panning now lives
    in the projection naturally. Density at the top (~45 px between
    adjacent lanes) approximates the launcher's original 17-line
    look. Horizontal scanlines stay decorative, no x-pan.
  * Track widened from `±1.5` → `±5` world units so obstacles spawn
    across the full playfield (matching Race The Sun).
- 2026-05-08 — Floor projection unified with the obstacle path so
  every visual element shares one pinhole camera. Several iterations
  on the same theme:
  * **Horizontal scanlines now world-anchored**, not screen-anchored.
    Replaced the launcher's evenly-spaced screen-y stripes (which
    didn't perspective-compress with depth) with stripes at fixed
    world-z = `k * FLOOR_LANE_L` projected via
    `sy = horizon_y + FLOOR_F / (k*L - cam_z)` — identical formula
    to `render_obstacles`'s base-y. `synthwave_step` now takes
    `dz_world` (= `ship_speed_z * dt`) instead of `scroll_pixels`,
    so the same world-z step that decrements every obstacle's
    `z_world` advances the floor's `cam_z` accumulator. cam_z is
    bounded modulo `LANE_L * FLOOR_HSTRIPE_DRAW_EVERY` for float
    stability, with the wrap window deliberately a whole multiple
    of the *drawn* stride so the modulo filter `k % DRAW_EVERY == 0`
    keeps surviving stripes continuous across wraps.
  * **Stripe density factor** `FLOOR_HSTRIPE_DRAW_EVERY = 3` — at
    default speed the raw 1-stripe-per-world-unit cadence is
    `ship_speed_z` Hz at the bottom of the screen which strobes;
    drawing every 3rd stripe drops it to ~4 Hz without changing the
    underlying world-z anchoring (an obstacle and a stripe at the
    same world-z still align).
  * **Vertical lane top-endpoint bug fixed**: the lane line was
    being drawn from `(HALF_W + F*dx/Z_TOP, horizon_y)` to
    `(HALF_W + F*dx/Z_NEAR, GRID_BOTTOM_Y)`. The top sx was
    computed at z=10 but the top sy at z=∞ — projection
    inconsistent with itself, so any obstacle at `world-X = X`
    visibly drifted off the lane line as cam_x moved. A constant-X
    ray on the ground projects to a *straight* screen-space line
    through the vanishing point `(HALF_W, horizon_y)` (because
    `sx-HALF_W = F*dx/z` and `sy-horizon = F/z` are both linear in
    `1/z`). Fixed: top endpoint = vanishing point exactly, bottom
    computed at `FLOOR_Z_NEAR` for *both* sx and sy. Now obstacles
    at integer world-X land exactly on the corresponding lane line
    at every depth.
  * **Vertical lane kx range extended**: was capped at
    `±(HALF_W * Z_NEAR / F)` ≈ ±1.78 world units, which left out
    every lane that's only visible near the horizon. As cam_x
    panned, those off-screen lanes "popped in" at the screen edges.
    New cap is `±(HALF_W * Z_FAR / F)` ≈ ±53 world units —
    `pax_simple_line` clips to the framebuffer when it actually
    rasterizes, so the cost of the off-screen lines is small.
  * **Debug speed knob** wired through `input_consume_speed_delta()`
    + a `v=NN.N` readout in `main.c` so the floor/obstacle motion
    can be inspected at arbitrary speeds. Cursor up/down step
    `game.ship_speed_z` by ±1 u/s (clamped 0.5..60). Speed text is
    positioned via `pax_buf_get_widthf(&fb)` so it sits flush at
    the right edge of the orientation-corrected logical width
    (using raw `display_h_res` puts it near screen centre because
    that's the un-rotated physical width).
  * **Stripe sy now float**, dedup on `int(sy)`. `pax_simple_tri`
    used by obstacles takes float vertices, so keeping the floor
    stripe's sy as a float (instead of int-floor before pass-down)
    keeps the rasterized pixel rows identical between an obstacle's
    base edge and a stripe at the same world-z.
- 2026-05-08 — Obstacles upgraded from flat front-face billboards to
  full 3D cubes with per-obstacle geometry and palette, plus a
  continuous side wall on each edge of the track:
  * **`obstacle_t` now carries its own dimensions and colours**
    (`half_w`, `half_d`, `height`, `front_color`, `side_color`,
    `top_color`, `outline_color`). The renderer no longer special-
    cases obstacle types — pickups, walls, future tall/short
    variants are all the same draw path with different fields. The
    old `OBSTACLE_*_COLOR` and `OBSTACLE_HEIGHT/HALF_W` constants
    moved into `world.c` as defaults applied at spawn time.
  * **Cube renderer**: each obstacle projects 8 corners and draws
    front face plus one side face (left/right chosen by `cam_x`
    relative to the cube's x extent) plus top face (only if
    `RENDER_CAM_Y > height`, currently false at the default 2-unit
    height with cam_y=1, but the path is in place for future low
    pickups). Side colour is a halved-magenta variant of the front
    colour so the depth reads as differently-lit faces.
  * **Side walls** (`WALL_X_RIGHT/_LEFT = ±5.5`, half_w=0.5,
    height = OBSTACLE_HEIGHT/3, half_d = 1.5). Each wall is a
    chain of 3-world-unit-long segments stored as regular
    `obstacle_t` entries — collision in Phase 4 will hit them
    automatically. Segment length matches the floor's drawn-stripe
    stride (`FLOOR_LANE_L * FLOOR_HSTRIPE_DRAW_EVERY`) and centres
    are offset by half a stride so each segment runs from one
    drawn grid line to the next. World tracks two per-side
    `*_wall_far_z` cursors that get topped up deterministically as
    the camera advances.
  * **Pool bumped 64 → 128** to fit ~33 wall segments per side
    plus ~30 dynamic obstacles plus headroom for Phase 9 pickups.
    `world` moved to a `static` local in `app_main` so the ~5 KB
    pool lives in bss instead of the IDF default stack.
  * **Despawn now compares the back edge** (`z_world + half_d`)
    against the threshold instead of the centre. The old centre-
    based test fired while a long cube's back edge was still on
    screen — short obstacles passed cleanly because their back
    edge was already off-screen at the same `z_world`, but a 3-
    unit wall segment despawned with its tail at sy≈470 and
    flickered out. Comparing the back edge makes the rule depth-
    invariant.
  * **Near-plane clipping** at `NEAR_CLIP_Z = 0.5`. Long obstacles
    that haven't despawned yet can have a negative `zF`; the old
    `if (zF < 0.05) zF = 0.05` clamp made the projected front edge
    enormous (F/0.05 = 9000 px/world-unit) which produced bogus
    side-face triangles climbing the screen. Now: if `zB <
    NEAR_CLIP_Z` the whole cube is dropped; if `zF_raw <
    NEAR_CLIP_Z` we set `front_visible = false` (skipping the
    front face) and use `zF = NEAR_CLIP_Z` for the side and top
    faces' front edges so all projected vertices stay at well-
    defined screen coords.
  * **Outline drawn edge-by-edge**, not face-by-face. The 12
    cube edges are listed explicitly with each edge gated on
    whether it bounds any visible face. Earlier per-face logic
    that put the front face's left/right verticals inside the
    side-face branches dropped one of them whenever only one
    side was visible — typical for tall pillars seen edge-on —
    and the user noticed the missing back-vertical edges.
- 2026-05-08 — Ship redesigned: bank-driven flight model + 3D
  tetrahedron mesh.
  * **Lateral physics**. `ship_vx_world` is gone. State now
    carries a single signed `bank ∈ [-1, +1]` factor; lateral
    velocity is purely `bank * SHIP_TURN_RATE` so a smoothly
    ramping bank yields smoothly ramping turn — no friction
    model needed. Each frame the bank moves toward
    `target = (float)steer` at `BANK_ACTIVE_RATE = 3.5/s` when
    any direction is held (including the opposite of the
    current bank, so reversing snaps the ship over fast) and at
    `BANK_PASSIVE_RATE = 1.0/s` when the stick is released. The
    asymmetry is what makes "let go and drift back" feel
    different from "pull the other way" — passive return takes
    ~1.0 s for a full straighten while a stick reversal does
    the full flip in ~0.57 s. Visual roll = `bank * MAX_BANK_RAD`
    (= 0.55 rad ≈ 31°), kept moderate so the wing tips don't
    clip the ground at SHIP_BASE_Y.
  * **3D mesh**: the screen-space placeholder (3 flat triangles)
    is replaced with a 4-vertex tetrahedron — nose lifted as the
    apex, two wing tips and the tail at ground level. Projected
    through the same pinhole camera as obstacles, with a roll
    about the +z axis driven by `bank`. The first attempt used a
    5-vertex flattened-diamond-with-cockpit, but the cockpit
    vertex projected interior to the silhouette, which made the
    two back-half tris span the entire left/right halves of the
    silhouette and paint over the smaller front-half tris; only
    the back tris were visible. Tetrahedral design has every
    vertex on the silhouette, so the two roof panels split the
    screen along the nose-tail centerline and never overlap.
  * **Per-face shading**: left and right roof panels get
    different yellows (sun-yellow + dimmer yellow), so the
    centerline ridge reads as a sharp colour break without
    needing an outline. Belly is magenta and only shows when
    banking exposes the underside.
  * **Cyan ridge + silhouette outlines** drawn over the fills:
    nose↔Lwing↔tail↔Rwing perimeter plus the nose↔tail dorsal
    ridge. Same cyan as the obstacle wireframe so the visual
    style stays consistent across all 3D objects.
  * **Position**: `SHIP_Z_PLANE` 2.5 → 2.0 (ship closer to
    camera, bigger on screen but still small at 0.55× the
    earlier mesh scale) and `SHIP_BASE_Y` 0.5 → 0.22, which
    drops the projected tail from sy ≈ 436 to sy ≈ 470 — just
    above the screen edge — exactly where the user wanted the
    ship visually anchored.

---

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
  optional custom-seed practice mode** so the player can replay the same
  level deterministically.
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

### `world.c` — daily seed, regions, content streams

- `world_init(uint32_t daily_seed)` — seeds an xorshift32 PRNG state; chooses
  region order/parameters for the day; immediately fills the side
  walls so they're visible on the first frame.
- `world_advance(world_t*, float dt, float ship_speed)` — moves the
  camera-z forward; despawns obstacles whose *back edge* has crossed
  the near threshold; tops up the side-wall cursors so each side
  remains a continuous chain of segments out to the far spawn plane;
  spawns dynamic obstacles on the randomized cadence.
- Obstacles stored in a fixed pool (128 entries) of
  `{ x, z, half_w, half_d, height, colors, active }`. Each entry
  carries its own dimensions and four-colour palette — the
  renderer doesn't distinguish dynamic obstacles from wall
  segments from future pickups, they're all just cubes.
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
- Pickup pool (32) of `{ x, y, z, kind }` — kinds: `TRI`, `BOOST`, `JUMP`,
  `SHIELD`, `CHECKPOINT`.
- 7 regions per run (mirrors the original). Each region is ~30s of game
  time; difficulty/density/mutators are looked up from a static per-region
  table indexed by region number, then perturbed with the PRNG.
- **Seed sources** (priority order, picked at title screen):
  1. **Daily seed** (default): `seed = year*10000 + month*100 + day` from
     `time(NULL)` / `localtime()`. If the year is < 2024 the RTC is unset
     — fall back to a stored "last known good" date in NVS so the player
     can't farm yesterday's seed by rolling the clock backwards.
  2. **Custom seed** (practice mode): player enters a seed on the title
     screen via a "Custom Seed…" menu item. Used for replaying the same
     world repeatedly. **No meta-progression is awarded in custom-seed
     runs** (no challenge points, no level-up, no unlock toward
     attachments) — but the highscore *for that specific seed* is tracked
     in a small ring of recent custom-seed best scores. This prevents the
     player from gaming the meta-progression by replaying easy seeds, but
     still rewards mastery.
- The seed mode is set by `world_init(seed, is_custom)`; downstream
  modules (notably `meta.c`) check the `is_custom` flag before awarding
  challenge progress.

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
- Collision: AABB in 3D world space against obstacle pool, plus pickup
  proximity test that respects magnet upgrade range.
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
- Multiplier: every 5 Tris collected → +1; crash drops by 5; floor is
  determined by player level (lv6→2, lv12→3, lv23→4, lv24→max).

### `render.c` — projection and scene draw

- Frame entry, **explicit per-frame draw order** (matters for occlusion):
  1. `synthwave_draw_sky(fb)` — purple background fill
  2. `synthwave_draw_sun(fb, sun_dy)` — pre-triangulated sun bands, shifted
     downward as the sun-timer drains. Drawn **before** mountains so the
     mountain silhouette occludes the lower half of the sun naturally as
     it sinks. `sun_dy = (1.0 - sun_seconds_left/SUN_MAX) * SINK_RANGE`.
  3. `synthwave_draw_mountains(fb)` — pre-triangulated mountain polygon
  4. `synthwave_draw_wireframe(fb)` — cyan mountain lines
  5. `synthwave_draw_top_grid(fb)` — single magenta horizon line
  6. `synthwave_step(fb)` — animated grid floor; scroll speed modulated by
     ship speed (caller advances the internal `j` counter by a variable
     amount so faster ship = faster lines).
  7. 3D scene (obstacles, pickups, painter-sorted by z, back-to-front)
  8. Ship sprite (with shadow tint applied if `game.in_shadow`)
  9. HUD (score, multiplier, region, pickup inventory, optional volume bar)
- 3D projection: pinhole camera at `(0, 1.0, 0)` looking down +Z. World
  coordinates: x = lateral, y = vertical, z = depth (positive = forward).
  Project: `sx = HALF_W + (x - cam_x) * f / z`, `sy = HALF_H - (y - cam_y)
  * f / z`. `f` chosen so 800px-wide screen frames a comfortable lateral
  FOV. Render obstacles as 4–8 triangles each (front face + sides),
  back-to-front sorted by z (no z-buffer). Use `pax_simple_tri()` from
  `include/shapes/pax_tris.h`.
- Ship: rendered as 3 triangles in screen space (no projection — fixed at
  bottom of screen, only its `x` moves).
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

### `meta.c` — levels, challenges, persistence

- Static table `level_unlocks[26]` mapping level → unlock-flag enum
  (BOOST_PICKUP, MULTIPLIER, JUMP, MAGNET, …, APOCALYPSE, etc.) per the
  original-game research. Implemented as a `const struct { ... }`
  initialized at file scope.
- `challenge_template_t templates[~14]` — types: REACH_REGION, COLLECT_TRIS,
  TRAVEL_DISTANCE, USE_PICKUP, REACH_MULTIPLIER, PERFECT_REGIONS,
  ONLY_LEFT, ONLY_RIGHT, with `(min,max)` ranges per tier.
- `meta.active[3]` — three concurrent challenges (1pt, 2pt, 3pt). On
  completion: award points, reroll that slot from the **daily** PRNG
  seeded by `today_seed * 7 + slot_index`. So challenges are also
  date-stable.
- Persistence (NVS namespace `synthracer`):
  - `level` (u8) — current player level 1..25
  - `points` (u8) — challenge points toward next level
  - `unlocks` (u32) — bitmask of feature flags
  - `attach1` / `attach2` (u8) — equipped attachment IDs
  - `highscore` (u32) — all-time best (across all seed modes)
  - `last_date` (u32) — last seen RTC date as `yyyymmdd`
  - `ch_state` (blob, ~64B) — three challenge slots' current targets and
    progress
  - `daily_done_<date>` (u8) — bitmask of which challenges done today
    (lazy-cleaned: when `last_date` rolls forward, drop the oldest key)
  - `last_custom_seed` (u32) — most recent custom seed entered (UI default)
  - `cs_best` (blob) — small ring of `{seed, score}` pairs (e.g. last 8
    custom seeds played) so the title screen can show "Best on this
    seed: 12345" when the player re-enters a seed they've played before.
- **Custom-seed runs do NOT touch** `level`, `points`, `unlocks`, or
  `ch_state` — only `highscore` (if beaten) and `cs_best`. `meta.c`'s
  pickup/region callbacks check the `is_custom` flag and early-return.
- API: `meta_load()`, `meta_save()`, `meta_on_pickup_collected()`,
  `meta_on_region_complete()`, `meta_on_run_end(score)`,
  `meta_reroll_for_date(seed)`.

### `audio.c` — SFX + system volume

- I2S mixer task pinned to core 1 (mirroring floppybird, lines 139–194).
- Sample slots: 4 simultaneous voices, mono → stereo expansion.
- Embedded SFX in `sfx.h` (generated by `xxd -i` from PCM raw 16-bit
  16 kHz mono): `sfx_pickup_tri`, `sfx_pickup_special`, `sfx_crash`,
  `sfx_boost`, `sfx_jump`, `sfx_shield`, `sfx_levelup`, `sfx_sunset`.
  Total ~80 KB embedded.
- System-volume handling, **mirroring `tanmatsu-launcher/main/global_event_handler.c`**:
  - On boot: read audio-jack state via
    `bsp_input_read_action(BSP_INPUT_ACTION_TYPE_AUDIO_JACK, …)`. Open
    NVS namespace `"system"`, read `"speaker.volume"` or `"hp.volume"`
    (default 50). Call `bsp_audio_set_amplifier(!hp_inserted)` and
    `bsp_audio_set_volume(percent)`.
  - On VOLUME_UP / VOLUME_DOWN navigation key (state=pressed): step ±5,
    clamp 0..100, write back to the appropriate `"system"` NVS key, call
    `bsp_audio_set_volume(percent)`. Optionally flash a small overlay
    bar in `render.c` for a second.
  - On `BSP_INPUT_ACTION_TYPE_AUDIO_JACK` action: update jack state,
    re-read NVS for the new output, re-apply both volume and amplifier.
  - **Important**: this writes to the launcher-shared `"system"`
    namespace, so the volume change persists across apps — exactly what
    the user wants.

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

1. NVS init, BSP init (RGB888 framebuffer, num_fbs=1 — same as template).
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

## Input Mapping

### Original *Race The Sun* controls

| Action | Keyboard | Gamepad |
|---|---|---|
| Steer left | A / Left arrow | D-pad left / left stick |
| Steer right | D / Right arrow | D-pad right / right stick |
| Use pickup (jump / shield / boost) | Space | A (south button) |
| Barrel roll (defensive) | Rapid alternating left↔right input | Same |
| Pause | Esc | Start |
| Menu confirm | Enter / Space | A |
| Menu cancel / back | Esc | B |
| Throttle | (none — automatic acceleration) | (none) |

### Tanmatsu mapping (Race the Synth)

The Tanmatsu has a full QWERTY keyboard, a D-pad/navigation cluster
(LEFT/RIGHT/UP/DOWN), gamepad face buttons (A/B/X/Y), F1–F12, and
volume keys. We use polled reads for steering (smoother than waiting on
event repeats) and the event queue for everything else.

| Action | Tanmatsu input | Read mode | Notes |
|---|---|---|---|
| Steer left | `BSP_INPUT_NAVIGATION_KEY_LEFT` **or** keyboard `a` **or** `BSP_INPUT_SCANCODE_ESC` (in-game only) | Polled (`bsp_input_read_navigation_key`) and polled scancode | Both edges accepted; held = continuous turn. ESC is the left thumb-rest key — ergonomic for long sessions. |
| Steer right | `BSP_INPUT_NAVIGATION_KEY_RIGHT` **or** keyboard `d` **or** `BSP_INPUT_SCANCODE_BACKSPACE` (in-game only) | Polled | Backspace is the right thumb-rest key — ergonomic mirror to ESC. |
| Use pickup | `BSP_INPUT_SCANCODE_SPACE` **or** `BSP_INPUT_NAVIGATION_KEY_GAMEPAD_A` | Event (press edge) | Cycles through inventory: jump → shield → checkpoint, whichever the player has. Or we expose three slots — TBD; default to jump-first. |
| Barrel roll | Detected in `input.c` from rapid LEFT-edge → RIGHT-edge (any of the three left/right bindings) within 250 ms | Event timing | Cosmetic + small dodge invulnerability frame. Optional polish — defer to Phase 13 if implementation is hairy. |
| Pause | `BSP_INPUT_NAVIGATION_KEY_F4` | Event (press edge) | First press → pause overlay; second press resumes. **Note**: ESC was originally planned for pause, but is now reused as the left-steer thumb key during play; F4 is a free function key with no other role. |
| Title-menu navigate | `BSP_INPUT_NAVIGATION_KEY_UP` / `_DOWN` | Event (press edge) | |
| Title-menu confirm | `BSP_INPUT_SCANCODE_ENTER` **or** `_SPACE` **or** `GAMEPAD_A` | Event (press edge) | |
| Title-menu cancel | `BSP_INPUT_SCANCODE_ESC` **or** `GAMEPAD_B` | Event (press edge) | ESC is steering only **during PLAYING**; in menus it reverts to its conventional cancel role. |
| Custom-seed entry | Digit keys `0`–`9` from `INPUT_EVENT_TYPE_KEYBOARD.ascii` | Event (ASCII) | Backspace edits text, Enter confirms, Esc cancels — Backspace and Esc revert to their conventional roles inside the seed-entry dialog (no steering active there). |
| Volume up | `BSP_INPUT_NAVIGATION_KEY_VOLUME_UP` | Event (press edge) | Updates `system/speaker.volume` or `system/hp.volume`, applies via `bsp_audio_set_volume()`, briefly shows a HUD volume bar |
| Volume down | `BSP_INPUT_NAVIGATION_KEY_VOLUME_DOWN` | Event (press edge) | Same |
| Audio-jack toggle | `BSP_INPUT_ACTION_TYPE_AUDIO_JACK` | Event (action) | Re-reads NVS for the new output, calls `bsp_audio_set_amplifier(!hp_inserted)` and `bsp_audio_set_volume()` |
| Restart to launcher | `BSP_INPUT_NAVIGATION_KEY_F1` | Event (press edge) | Always available; matches template convention |
| Backlight dim / bright | `F2` / `F3` | Event (press edge) | Matches template convention |
| Power button | `BSP_INPUT_ACTION_TYPE_POWER_BUTTON` | Event (action) | Treated as exit-to-launcher (mirrors videoplayer) |

### Modal binding: ESC and Backspace

ESC and Backspace have **state-dependent meaning**, decided in
`input.c` based on `game.state`:

| State | ESC behavior | Backspace behavior |
|---|---|---|
| `STATE_TITLE` | menu cancel / back | (unbound, ignored) |
| `STATE_MENU_SEED` (custom-seed dialog) | cancel dialog | edit / delete digit |
| `STATE_PLAYING` | **steer left** (polled) | **steer right** (polled) |
| `STATE_PAUSED` | resume play | (unbound) |
| `STATE_GAME_OVER` | (unbound) | (unbound) |

This is unambiguous — the player is never steering during a menu and
never editing text mid-run, so the role swap never collides with itself.
The modal logic lives entirely in `input.c`'s polled-read path:
`is_steer_left = nav_left_held || a_held || (state==PLAYING && esc_held)`.

### Mapping rationale

- **Polled steering** matches `tanmatsu-placeinvaders-grace/main/main.c`
  (`bsp_input_read_navigation_key()` for LEFT/RIGHT). Event-queued
  steering depends on repeat-rate quirks and produces stutter.
- **Three steering options per side** (D-pad, WASD, Esc/Backspace)
  because the Tanmatsu's grip varies by player: D-pad for one-handed
  play, WASD for keyboard-style two-handed grip, and Esc/Backspace as
  thumb-rest keys at the corners of the keyboard for long-session
  ergonomics. Cost is three polled reads per side per tick — trivial.
- **Single "use pickup" button** mirrors the original — Race The Sun has
  one action button that consumes whichever pickup is at the head of the
  inventory queue. Cleaner than dedicated jump/shield/checkpoint keys.
- **Barrel roll detection** is *not* on the critical path. Phase 1–8
  ship without it; Phase 13 polish adds it if the timing window is
  tunable enough to feel right.
- **F1 always exits** — convention from template and every sibling
  graceloader game. **Pause moved to F4** so ESC can serve as the
  left-thumb steering key during play.
- **Volume keys are global** — mirrors launcher and videoplayer; writes
  to the shared `"system"` NVS namespace so the change persists across
  apps. Confirmed by reading
  `tanmatsu-launcher/main/global_event_handler.c:58–67` and the volume
  helpers at
  `tanmatsu-launcher/managed_components/nicolaielectronics__tanmatsu-settings/src/nvs_settings_hardware.c:32–46`.

### Things deliberately NOT mapped

- **Throttle / brake** — original has none, we don't either. Speed is
  managed by the sun timer + boost pickups + shadow slowdown.
- **Camera control** — fixed third-person chase camera. No swing.
- **Joystick stick press** (`BSP_INPUT_NAVIGATION_KEY_JOYSTICK_PRESS`) —
  reserved; ignored for now. Could map to "use pickup" as an alternate.
- **Multi-button chords** — none. Every action is a single key.

---

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
4. **Collision + game over**: AABB collision. Crash → STATE_GAME_OVER →
   back to STATE_TITLE on space.
5. **Sun timer + shadow + boost pickups**: tick `sun_seconds_left`, sink
   the visual sun (verify it disappears behind the mountain silhouette),
   end run on sunset. Implement `is_ship_in_shadow()` and the speed
   slowdown. Spawn boost pickups; collect → +time and speed reset.
6. **Tris + multiplier**: pickup pool extended, draw blue triangles,
   multiplier rule, score accumulator, HUD score readout.
7. **Audio**: spawn mixer task, embed first 4 SFX, trigger on
   pickup/crash/boost. Wire volume keys + audio-jack handling into the
   event drain. Test that volume persists to launcher.
8. **Daily seed + custom seed + persistence**: read RTC, derive daily
   seed, persist highscore/level/points to NVS namespace `synthracer`.
   Validate the "last known date" anti-cheat. Add the title-screen
   "Custom Seed…" entry dialog + `last_custom_seed` / `cs_best`
   persistence. Verify that custom-seed runs don't award meta-progression.
9. **Pickups & attachments**: jump, shield, checkpoint pickups +
   the attachment slots and magnet/battery upgrades.
10. **Regions**: 7-region progression with mutators, perfect-region flag,
    movement-restriction tracking.
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
   region). Verify a custom-seed run does NOT increment `points` or
   `level` even after a long survive.
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

## Appendix A — Race The Sun Research Reference

This is the offline-cached research summary from web sources, included so
future sessions don't have to re-fetch. Sources cited at end.

### Core gameplay loop

- Player pilots a solar-powered craft along an endless, procedurally
  generated landscape under a setting sun.
- **Steer left/right only.** Acceleration is automatic; speed is constant
  in direct sunlight.
- **Speed decreases in shadow** (large obstacles, clouds). Stay shadowed
  too long → stall.
- The **sun continuously sets**. When it drops below the horizon, the
  craft loses power and the run ends.
- **Speed-boost pickups** push the sun back up briefly.
- Optional advanced inputs: **jump** (consumable pickup), **barrel roll**
  (rapid alternating left/right). Original used jump to clear obstacles.
- **Direct collision = instant death** (unless a Shield is consumed).

### Pickups

| Pickup | Effect |
|---|---|
| Blue Tri (pyramid) | Multiplier +1 after every 5 collected. Crash drops it. |
| Speed boost | Temporary speed up + raises sun. |
| Jump | Single-use; ship hops up and floats down. Stackable up to 3 with upgrades. |
| Shield / Emergency Portal | Single-use; absorbs one fatal hit and teleports forward/up. |
| Checkpoint | (Late-game) checkpoint storage upgrade enables multiple. |

### Scoring

- Continuous accumulation while moving forward.
- Multiplier driven by Tris (5 → +1×). Higher meta levels raise the
  multiplier floor (lv6→2, lv12→3, lv23→4, lv24→max).
- Crashing drops multiplier by ~5.

### World structure

- Pseudo-procedural regions (~7), each with distinct themes/obstacle sets
  and difficulty mutators.
- World layout is the **same for all players for the day** — daily seed,
  shared leaderboards. Resets every 24 hours.
- Modes: **Standard** (default), **Apocalypse** (faster, less forgiving,
  unlocks at lv11), **Labyrinth** (top-down maze, unlocks at lv25).

### Metaprogression

Two intertwined progression layers:

1. **Per-run** — sequential regions, increasing speed/density. Multiplier
   builds over the run.
2. **Persistent player level (1 → 25)** — each level grants a permanent
   unlock; this is the meta layer.

#### Challenge system

- Always **3 active challenges**: 1-point, 2-point, 3-point.
- Complete one → immediately replaced by a new one.
- Procedural variations on a small template set:
  - Reach region N
  - Collect N Tris (sometimes "in one region", sometimes "air tris")
  - Use pickup type X N times
  - Travel total distance N
  - Reach N× multiplier
  - Perfect-region (no crash) — N regions in one run
  - Movement-restricted ("only turn left", "only turn right")
- Points required per level scales: ~3 at low levels, up to ~8 near top.

#### Per-level unlocks (canonical 1–25 ladder)

| Lvl | Unlock |
|----|----|
| 1  | (start) |
| 2  | Speed-boost pickup |
| 3  | Multiplier system (Tris → +1×) |
| 4  | Jump pickup |
| 5  | Magnet attachment |
| 6  | Starting multiplier 2× |
| 7  | Portal to easier alternate world |
| 8  | Double jump storage |
| 9  | Shield pickup |
| 10 | Shield attachment |
| 11 | **Apocalypse mode** (harder/faster) |
| 12 | Starting multiplier 3× |
| 13 | Second attachment slot |
| 14 | Left-wing decal |
| 15 | Double portal storage |
| 16 | Checkpoint pickups |
| 17 | Power-turning attachment |
| 18 | Second power-turning attachment |
| 19 | Triple jump storage |
| 20 | Enhanced checkpoint storage |
| 21 | Enhanced magnet |
| 22 | Right-wing decal |
| 23 | Starting multiplier 4× |
| 24 | Final multiplier upgrade |
| 25 | New mode (Labyrinth) + battery upgrade ("complete") |

### Sources (research date: 2026-05-07)

- https://en.wikipedia.org/wiki/Race_the_Sun_(video_game)
- https://store.steampowered.com/app/253030/Race_The_Sun/
- https://steamcommunity.com/sharedfiles/filedetails/?id=203298348 (basic guide)
- https://steamcommunity.com/sharedfiles/filedetails/?id=207229205 (level unlocks)
- https://www.gamespot.com/reviews/race-the-sun-review/1900-6413902/
- https://portforward.com/games/walkthroughs/Race-The-Sun/The-Rest.htm
- https://psnprofiles.com/guide/5414-race-the-sun-trophy-guide
- http://flippfly.com/racethesun/releasenotes/

---

## Appendix B — Tanmatsu API Reference (cached)

Cheat-sheet of the most relevant APIs found during exploration of the SDK
headers under `tanmatsu-synthracer-grace/include/`. Cited so we don't have
to re-grep next session.

### PAX graphics (2D only — no 3D math, hand-rolled projection required)

- `pax_buf_init(&fb, NULL, w, h, PAX_BUF_24_888RGB)` — `include/pax_gfx.h:94`
- `pax_buf_set_orientation()`, `pax_buf_get_pixels()` — same header
- 2D matrix stack: `pax_push_2d`, `pax_pop_2d`, `pax_apply_2d` — same header
- `pax_simple_tri/rect/line/circle` — `include/shapes/`
- `pax_draw_shape(buf, color, npts, points)` — triangulates each call.
- `pax_triang_concave(&indices, npts, points)` →
  `pax_draw_shape_triang(buf, color, npts, points, ntris, indices)` —
  pre-triangulate path. `include/pax_shapes.h:99,108`.
- `pax_draw_text`, `pax_center_text`, `pax_text_size` — `include/pax_text.h`
- Built-in fonts: `pax_font_sky`, `pax_font_sky_mono`, `pax_font_marker`,
  `pax_font_saira_condensed`, `pax_font_saira_regular` — `include/pax_fonts.h`
- `pax_clip(buf, x, y, w, h)` / `pax_noclip(buf)` for HUD scissoring.

### BSP — display, input, audio, LEDs

- `bsp_device_initialize(cfg)`; `bsp_device_restart_to_launcher()` — `bsp/device.h`
- `bsp_display_get_parameters()`, `bsp_display_blit(x,y,w,h,buf)` —
  `bsp/display.h:36, 79`
- Vsync: `bsp_display_set_tearing_effect_mode(BSP_DISPLAY_TE_V_BLANKING)` +
  `bsp_display_get_tearing_effect_semaphore(&sem)` then
  `xSemaphoreTake(sem, pdMS_TO_TICKS(50))`. Pattern from
  `tanmatsu-floppybird-grace/main/main.c:519–527, 748`.
- `bsp_input_get_queue(&q)` (event-driven), `bsp_input_read_navigation_key()`
  / `bsp_input_read_scancode()` / `bsp_input_read_action()` (polled) —
  `bsp/input.h:257, 273, 277, 281`.
- Nav key constants: `BSP_INPUT_NAVIGATION_KEY_{LEFT,RIGHT,UP,DOWN,F1..F12,
  GAMEPAD_A,B,X,Y,JOYSTICK_PRESS,VOLUME_UP,VOLUME_DOWN}` — `bsp/input.h:148–196`
- Action types include `BSP_INPUT_ACTION_TYPE_AUDIO_JACK`,
  `BSP_INPUT_ACTION_TYPE_SD_CARD`, `BSP_INPUT_ACTION_TYPE_POWER_BUTTON`.
- `bsp_audio_initialize(rate)`, `bsp_audio_get_i2s_handle(&h)`,
  `bsp_audio_set_amplifier(bool)`, `bsp_audio_set_volume(float pct)` —
  `bsp/audio.h`.
- `bsp_led_set_pixel(idx, rgb)`, `bsp_led_send()`, `bsp_led_set_mode(auto)`,
  `bsp_led_set_brightness(pct)` — `bsp/led.h`.

### NVS

- `nvs_flash_init()` — `nvs_flash.h:78`
- `nvs_open(ns, mode, &h)`, `nvs_close(h)`, `nvs_commit(h)` — `nvs.h:162, 590, 577`
- `nvs_set_u8/u16/u32/u64/i32/str/blob` and matching `nvs_get_*` —
  `nvs.h:233..501`. Key max 15 chars.
- **Shared "system" namespace keys** (used by launcher; we must use these
  for volume to integrate properly):
  - `"speaker.volume"` (u8 percentage 0..100)
  - `"hp.volume"` (u8 percentage 0..100, for headphones)
  - Helpers in launcher source at
    `tanmatsu-launcher/managed_components/nicolaielectronics__tanmatsu-settings/src/nvs_settings_hardware.c`.

### Time

- `time(NULL)` / `localtime()` / `clock_gettime(CLOCK_REALTIME, ...)` for
  wall-clock. `time.h`.
- `esp_timer_get_time()` returns int64 microseconds since boot —
  `esp_timer.h:223`. Use for frame timing and PRNG fallback seed.

### Reference projects (for patterns)

- `tanmatsu-launcher/main/synthwave.{c,h}` — synthwave backdrop +
  scrolling grid (we copy & refactor this).
- `tanmatsu-launcher/main/global_event_handler.c:19–67` — exact volume +
  audio-jack handling.
- `tanmatsu-floppybird-grace/main/main.c` — game loop, vsync, NVS
  highscore, audio mixer task, xorshift32 PRNG.
- `tanmatsu-placeinvaders-grace/main/{game,render,audio}.c, sprites.h,
  sounds.h` — multi-file layout, sprite/sound embedding.
- `tanmatsu-thecube-grace/main/{main,renderer}.c` — software 3D-ish
  rendering pattern, fixed-step pacing.
- `tanmatsu-videoplayer/main/main.c:769–787` — alternate
  volume-key handling pattern (in-app only).

---

## Appendix C — How to resume work in a new session

1. Open this file. The **Current Status** table at the top is authoritative.
2. Read the **Conventions / decisions log** for any post-plan decisions.
3. Pick the next ⬜ phase from the table and follow its description in the
   "Implementation Phases" section above.
4. After completing a phase: flip its row to ✅, append any new design
   decisions to the log, and commit (`DEVELOPMENT.md` lives in the repo).
5. If a phase produces a non-trivial deviation from this plan, edit the
   relevant Module Responsibilities section so the design stays current
   instead of letting the doc rot.

