# Decisions Log

> Append-only record of every design and implementation decision on Race the Synth, in chronological order. Part of the [dev docs](README.md) — see it for the index.

**Conventions / decisions log** (append-only as new decisions are made):
- 2026-05-07 — Project bootstrapped from `tanmatsu-template-grace`.
- 2026-05-07 — Volume keys must update the launcher-shared `"system"` NVS
  namespace, not a private one.
- 2026-05-07 — Custom-seed runs do not award meta-progression.
  *(Superseded 2026-05-15 — see below.)*
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
  arbitrary writes". The memcpy was ~1.15 MB / frame on RGB888.
  **Superseded 2026-05-11:** the single `backdrop_cache` was first
  fed by a non-blocking PPA SRM (single-cache version), then split
  into `sun_cache` + `mountain_cache` and driven by a 3-op PPA
  FILL → SRM → BLEND pipeline so the sun can move independently
  of the mountains for Phase 5. See the 2026-05-11 decisions-log
  entries for the full history.
- 2026-05-07 — Phase 3 implementation landed: `world.{c,h}` (fixed
  pool of 64 obstacles, xorshift32 PRNG, randomized z-spawn cadence
  in world units so spawn density is speed-independent) and
  `render.{c,h}` (pinhole projection at horizon y=256, painter's
  algorithm via insertion-sorted z-descending index list, front-face
  triangle pair + cyan outline per obstacle). `synthwave_step` now
  takes a float `scroll_pixels`, accumulates internally, and the main
  loop drives it from `scroll_px_per_world_unit *
  game.ship_speed_z * dt` — floor scroll and obstacle approach now
  share the same forward-speed source. (World seed source has
  since moved to the RTC-derived daily seed — see the
  2026-05-11 entries below.)
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
- 2026-05-11 — Phase 4 implementation landed. State machine in
  `main.c`: `STATE_TITLE` / `STATE_PLAYING` / `STATE_GAME_OVER`,
  with `input_set_mode` driving the modal ESC/Backspace bindings
  and `input_consume_pickup` handling Space/Gamepad-A transitions.
  Title and game-over screens reuse the cached synthwave backdrop
  (gentle scroll on title, frozen on game-over) and draw centred
  Hershey overlay text. Each PLAYING frame does
  `game_step → game_collide → game_after_collide → world_advance`
  in order — motion produces this frame's intent, collide resolves
  it (push-out + flags + head-on return), after_collide reads the
  resolved flags for speed dynamics, world then advances. The
  debug speed knob now adjusts `ship_base_speed_z` (not the
  current `ship_speed_z`) so the scrape decay/recovery doesn't
  fight the player's tuning.
- 2026-05-11 — Collision classifier evolved through several
  shapes before landing in its final form. The axis-of-smallest-
  penetration rule (first attempt) misfired on long wall segments
  that had drifted past (z_pen shrunk below x_pen, faked a head-
  on) and on cube corner clips (allowed survivable graze past
  obstacles). An aspect-ratio shortcut was tried and rejected.
  A position-based variant — "boundary obstacle if its x range
  sits entirely outside the ship's lateral clamp" — fixed the
  wall transition and made cube corner clips fatal, but it baked
  the wall/non-wall distinction into the geometry of where the
  obstacle is placed, which doesn't generalise to future
  in-playfield scrape-able surfaces.
- 2026-05-11 — **Final form: per-obstacle `kind` tag.** Added
  `obstacle_kind_t` (`CUBE`, `WALL`, plus stubs for
  `PICKUP_TRI/BOOST/JUMP/SHIELD` and `RAMP`) and an `obstacle_t.kind`
  field. World's spawn helpers tag entries at creation time:
  `try_spawn_dynamic` → `CUBE`, `top_up_wall` → `WALL`. The collision
  classifier now dispatches on kind: WALL is always scrape, CUBE
  is head-on if ahead and trailing-scrape if past, pickup/ramp
  stubs `continue` so their future presence in the pool doesn't
  collide. The position-based boundary test is gone — wall vs
  non-wall is a data fact, not a geometric inference. Adding a
  new obstacle kind is now: one enum value + one case in
  `game_collide` + (when needed) one case in `render_obstacles`.
  Trade-off: 1 byte per pool entry (alignment-padded — already
  paying ~40 bytes per entry, +1 is noise) plus a small switch
  dispatch in collision. The benefit is that future obstacle
  types/shapes plug into the same general pool/render/collision
  logic the user asked for, instead of competing with the
  classifier's heuristics.
- 2026-05-11 — Collision model for Phase 4 derived from a
  playtest of the original *Race The Sun*: head-on collisions
  are fatal, but **scraping along a wall or the side of an
  obstacle gradually slows the ship while contact lasts and
  gradually re-accelerates back to base speed when contact is
  lost — never a step change.** Phase 4 will classify each AABB
  overlap by the axis of *smallest penetration*:
  * smallest on x → side contact → set a `scraping` flag.
    `ship_speed_z` ramps downward at a fixed deceleration (e.g.
    a few world-units / s²) toward a scrape-floor (~0.5-0.6 ×
    base). Releasing contact does not snap back — instead
    `ship_speed_z` ramps *up* at a separate acceleration toward
    `SHIP_BASE_SPEED_Z`. Both transitions are continuous, so
    grazing a wall for half a second only sheds a fraction of
    the full slowdown, and recovery is visible over time rather
    than instant.
  * smallest on z → head-on → STATE_GAME_OVER.
  Walls (long in z, thin in x) produce x-axis contact by
  geometry, so they're scrape-only without needing a special
  case. Small cube obstacles can be either depending on
  approach angle — clip a corner laterally and survive at
  reduced speed; hit it square-on and die. The "current forward
  speed" therefore becomes a value pushed around by several
  effects (scrape decel, Phase 5 shadow decel, Phase 5 boost
  reset, base-speed recovery accel) rather than a constant —
  landing the "speed responds to game state" pattern in Phase 4
  is a deliberate setup for Phase 5.

  **Scrape sparks** — visual indication of the contact, also
  landing in Phase 4 alongside the scrape model itself. Final
  form: a per-frame radial burst (no particle physics). For
  each scraping side, draw 5 red lines from the *rendered*
  wing-tip in random screen-space directions with random
  length 10-22 px. No pool, no lifetime, no advance — the
  burst is a pure per-frame visual that exists exactly while
  the scrape flag is set. The wing-tip world position has the
  same `bank * MAX_BANK_RAD` rotation about z applied as the
  ship's mesh, so the burst origin tracks the visible wing
  tip when banking (an earlier version projected the level-
  flight position and the sparks visibly drifted off the
  rendered wing tip).
- 2026-05-11 — **Stage / area world generation.** The world is
  split into `WORLD_STAGE_LENGTH_Z = 720` world-z stages (~60 s
  of forward travel at base speed), each followed by a
  `WORLD_REST_LENGTH_Z = 120` rest area (~10 s, no obstacles
  today; future home of Phase 5 bonus pickups). Inside a stage
  the world picks **obstacle areas** uniformly at random from
  the types whose `min_stage <= current stage`; an area spawns
  obstacles according to its own internal cadence until its
  length budget is consumed, then the world picks the next one.
  When the stage budget is exhausted *after* the current area
  finishes, the rest area runs and the stage counter ticks
  over. Areas always run to completion — stages may overshoot
  the budget slightly so the player never sees a clipped area.

  Determinism comes from a per-stage PRNG mixed from
  `(level_seed, stage_index)` via xorshift, derived freshly at
  each stage transition. A given run-seed reproduces every
  stage identically. Re-running the same seed → identical
  worlds in every stage.

  Initial area-type set, all min_stage 1 (random uniform draw):
  * `AREA_TYPE_PIXEL_FIELD` — the current small-magenta cube
    stream. Min length 2 screens (116 u), max 4 screens (232).
    Spawn interval 12-22 u at stage 1; scales -5% per stage
    above 1 to a 0.5× floor (reached at stage 10).
  * `AREA_TYPE_BIG_BLOCKS` — 2× lateral / 2× depth cubes,
    grey palette, same height. Sparser cadence (20-35 u
    base) to keep the track navigable. Otherwise identical
    to pixel field — collides as `OBSTACLE_KIND_CUBE` so
    head-on rules and rendering are unchanged.
  * `AREA_TYPE_GATEWAYS` — wall slabs spanning the full
    playfield width with a single ship-sized opening. Each
    gate is two `OBSTACLE_KIND_CUBE` slabs (left + right of
    the gap), amber palette, so head-on into a wall slab is
    fatal. Gap width lerps 3.0× → 1.5× ship width over
    stages 1-10 (clamped past 10) — that's the entire
    difficulty curve. Inter-gate / lead-in / trailing pad is
    a fixed 50 u for all stages; alternating hard-left ↔
    hard-right gaps need that much z to be reachable at
    cruise speed. Gate count per area is a uniform 1..5 draw.
  * `AREA_TYPE_REST` — internal-only, can't be picked by the
    area picker; only the stage rollover inserts it.

  Layout structure inside a gateway area:
  `[settle][pad][gate][pad][gate]...[gate][pad]`. The
  trailing pad falls out naturally from the length budget
  after the last gate spawn. Inter-gate gap is exactly `pad`
  because consecutive spawns are at the same far-plane z, so
  one pad of camera travel between events leaves one pad of
  empty world between the gates.

  Big-block and pixel-field cubes are tagged
  `OBSTACLE_KIND_CUBE` (not a new kind) — the only difference
  is dimensions and colour, which the renderer already pulls
  per-entry. Gateway slabs are also `OBSTACLE_KIND_CUBE` so
  head-on death works without a new collision case.

  Trade-off: lazy generation (area emits the next obstacle
  only when its spawn cursor crosses zero) rather than eager
  pre-queueing. Lazy is simpler, memory-bounded, and matches
  what the earlier `next_spawn_z` cadence already did. Means
  re-spawning the same `(seed, stage)` produces an identical
  area sequence but not an identical pool state at any given
  *world-z* — only at each area boundary. Acceptable.
- 2026-05-11 — **Gateway settling pad.** The gateway area
  starts with a `GATEWAY_SETTLE_Z = WORLD_Z_FAR_SPAWN` (100 u)
  hard wait before the alignment pad begins counting. The
  previous area can spawn its last obstacle right at the area
  boundary, placing it at camera-z=100; one full far-plane
  distance of camera travel guarantees that obstacle has
  crossed the camera before the first gate spawns. So the
  entire gateway area — including its lead-in pad — is free
  of drifting leftovers and the player only has the gate
  itself to focus on. Reads visually as a deliberate breath
  before the alignment puzzle. (Earlier version without
  settle had a soft guarantee: pad alone, with the previous
  area's last obstacle potentially visible during the
  approach. User asked for the hard guarantee.)
  *(Superseded 2026-05-15 — the settle was removed; see the
  "Gateway dead-air trimmed" entry below.)*
- 2026-05-11 — **Daily seed source landed** (partial Phase 8).
  `derive_daily_seed()` in main.c builds the world seed from
  the RTC date — `year*10000 + month*100 + day` — captured
  once at app boot. Same calendar day → same seed → identical
  world across run-restarts and app reboots; only rolls over
  at local midnight. The seed is hoisted out of `start_run()`
  so it's stable across die-and-retry inside one session.
  Fallback for an unset RTC (year < 2024) is a fixed constant
  `1` — the user can still play, just deterministically, until
  the clock is set. Phase 8's custom-seed menu is still TODO.
  Trade-off: the seed is captured at
  boot, so playing across midnight keeps the old seed until
  app restart. Deliberate — no surprise mid-run.
- 2026-05-11 — **Swept-z collision + kinematic classifier.**
  Player reported flying through obstacles aimed dead-centre.
  Root cause: the classifier looked at the obstacle's *current*
  `z_world` only — when per-frame `dz = ship_speed_z * dt`
  exceeded the AABB overlap window (~0.74 u for pixel cubes,
  ~0.68 u for the ship), the obstacle could skip from "ahead
  of ship" to "past ship centre" in a single frame. The
  current-frame view then said "obstacle past ship → trailing
  scrape" and the ship walked through it. At base speed
  (12 u/s) the window only holds for perfect 60 fps timing;
  one dropped frame, or any debug-knob speed bump, was enough
  to tunnel.

  Fix: `game_collide` now takes `dt` and does two things:
  1. **Swept-z overlap**: the z range tested runs from the
     obstacle's current near face (`z - half_d`) to its
     *previous* far face (`z + half_d + dz`). Even if the
     obstacle has fully passed the ship this frame, the swept
     range still overlaps so collision fires. No tunneling
     past the ship is silent any more.
  2. **Kinematic head-on / scrape classifier**: head-on iff
     `obs_zN_prev >= ship_zF` — the obstacle's near face was
     still ahead of the ship's front face at the *start* of
     this frame, i.e. the obstacle slammed into the ship from
     ahead during the frame. Doesn't care where the obstacle
     lands in the current frame, so a fast head-on dive that
     skips the overlap window still classifies as fatal.
     Trailing scrape only fires when the obstacle was already
     overlapping or past last frame (the genuine "obstacle
     drifting past us" case).

  X-axis tunneling is not possible: the ship moves laterally
  at ≤ SHIP_TURN_RATE * dt ≈ 0.058 u/frame, far smaller than
  the lateral overlap window, so x_pen stays a current-frame
  quantity and the push-out still resolves correctly. Only
  the z axis got the swept treatment.

  This supersedes the earlier "obstacle.z_world > SHIP_COLLISION_Z_C"
  rule used in the previous decisions-log entry; the
  per-obstacle kind tag is still the data-driven dispatcher,
  the swept-z + kinematic test is the new geometry inside
  the CUBE case. WALL still scrape-only, pickup/ramp stubs
  still `continue`.

- 2026-05-11 — **Performance baseline + RGB565 switch.** First
  on-device frametime measurement. Added per-phase
  instrumentation in `main.c`'s loop using
  `esp_timer_get_time()`, summing microseconds over ~1 s
  windows and logging FPS + phase breakdown. Phases: `input`
  (event drain + polled steering), `phys`
  (game_step/collide/after/world_advance, only in PLAYING),
  `bgcpy` (full-FB memcpy from backdrop_cache to fb),
  `bgflr` (synthwave_step — floor rect fill + lane lines +
  horizontal stripes), `obs` (render_obstacles — 3D cubes +
  wireframes), `fgrest` (ship + sparks + HUD text + state
  overlays), `blit` (bsp_display_blit DMA queue),
  `vsync` (semaphore wait — idle headroom).

  **Baseline (RGB888, 800×480, 1.15 MB framebuffer):**

  | Phase | Title ms | Gameplay ms |
  |---|---|---|
  | input  | 0.05 | 0.07 |
  | phys   | 0.00 | 0.12 |
  | bgcpy  | 27.7 | 27.7 |
  | bgflr  | 26.7 | 26.7 |
  | obs    | ~1.6 | 47.0 |
  | fgrest | (in obs+rest) | 4.2 |
  | blit   | 0.78 | 0.74 |
  | vsync  | 0    | 0    |
  | **FPS**| **17.6** | **9.5** |

  Zero vsync headroom means the loop is fully CPU-bound — frame
  time exceeds the 16.67 ms 60 Hz budget by ~5×.

  Root-cause findings:
  - `bgcpy=27.7ms` — 1.15 MB PSRAM→PSRAM memcpy. PSRAM
    bandwidth bound (~41 MB/s effective for read+write).
  - `bgflr=26.7ms` — `synthwave_step` does three things every
    frame: (a) `pax_simple_rect(0xFF5D0B8B)` fill of the floor
    base — 800 × ~224 px of 24bpp writes to PSRAM, ~10–12 ms
    alone, (b) ~108 vertical lane lines (kx range derived from
    far-plane half-width ±53 world units, mostly converging at
    the vanishing point — many sub-pixel-wide segments and
    off-screen draws clipped after edge-walking), (c) ~10
    horizontal stripes.
  - `obs=47ms` (gameplay) — ~80 active obstacles × (6 face
    triangles + 12 wireframe lines) ≈ 480 triangles + ~1000
    lines. Lines go through `pax_simple_line`'s per-pixel
    setter (not the range-setter), so each wireframe pixel pays
    a function-pointer call.
  - PAX itself is **not** doing per-pixel format dispatch.
    `pax_get_setter()` is called once per primitive and returns
    a specialized 16/24-bpp setter; format conversion of the
    `pax_col_t` happens once per primitive, not per pixel.

  **First optimization: RGB565 framebuffer.** Single-line
  change at `main.c:149` —
  `requested_color_format = LCD_COLOR_PIXEL_FORMAT_RGB565`.
  Format dispatch at `main.c:165–175` already had the RGB565
  branch wired up; `pax_buf_get_size` follows automatically;
  `pax_buf_reversed` handles RGB565 endianness from
  `display_data_endian`. Backdrop cache built fresh at boot in
  the new format, so sun/mountain/wireframe rendering goes
  straight into RGB565 with no extra source-code changes.

  **After RGB565:**

  | Phase | RGB888 | RGB565 | Δ |
  |---|---|---|---|
  | bgcpy  | 27.7 | 17.1 | -38% |
  | bgflr  | 26.7 | 17.5 | -34% |
  | obs    | 47.0 | 46.0 | ~0% |
  | fgrest | 4.2  | 3.85 | -8% |
  | **FPS gameplay** | **9.5** | **11.8** | **+24%** |

  bgcpy + bgflr gains track halved bytes (PSRAM bandwidth and
  16-bit halfword stores vs. misaligned 3-byte stores). `obs`
  barely moved, which confirms that triangle/line rendering in
  the obstacle path is dispatch-overhead-bound, not
  pixel-write-bound — RGB565 doesn't help here because the
  per-line function-pointer call cost dominates over the actual
  store width.

  **Hershey direct renderer made format-aware.** The original
  `hershey_direct_set_pixel` in `main/hershey_font_direct.h`
  hardcoded a `* 3` byte stride and a 3-byte write pattern,
  which corrupted text rendering after the RGB565 switch.
  Refactored to use a `hershey_native_color_t` struct that
  carries both the RGB888 byte triple and a pre-packed RGB565
  halfword (byte-swapped if `reverse_endianness`). The pack is
  done **once per string** in `hershey_direct_draw_string`, not
  per pixel; `set_pixel` branches on `buf->type` —
  `PAX_BUF_16_565RGB` writes a uint16_t halfword to
  `buf_16bpp`, otherwise writes 3 bytes to `buf_8bpp`. The
  string-level pre-pack means the inner Bresenham loop pays
  one halfword store per pixel — slightly faster than the old
  RGB888 path even on 24bpp buffers.

- 2026-05-11 — **PPA pipeline plan for backdrop (next step).**
  PPA (Pixel Processing Accelerator) is the ESP32-P4 2D DMA
  engine, exposed via `driver/ppa.h`: three ops (`SRM`
  scale/rotate/mirror, `BLEND` alpha or color-key compositing,
  `FILL` solid-rect fill). Symbols now exported by fakelib
  after the upstream rebase. PPA cannot rasterize triangles or
  lines — the obstacle pipeline stays on CPU — but it is well
  suited to bulk framebuffer copies and rectangular fills.

  **Goal**: replace the per-frame `memcpy(fb, backdrop_cache, ...)`
  with an async PPA SRM op that copies only the
  *above-horizon* region (logical y ∈ [0, 257), the sky / sun /
  mountains / wireframe / top-grid band). The floor area
  (y ≥ 257) is no longer copied — `synthwave_step` already
  rewrites every pixel below the horizon with a rect fill +
  lane lines, so the read+write traffic for the floor region
  is pure waste. Per-frame bandwidth comparison:

  | Approach | Floor traffic / frame | Notes |
  |---|---|---|
  | Current (memcpy + rect overpaint) | 540 KB read + 1080 KB write = 1.6 MB | 540 KB read is discarded |
  | Bake floor into backdrop cache | 540 KB read + 540 KB write = 1.1 MB | simpler, no separate fill |
  | PPA SRM above-horizon + CPU floor | 0 read + 540 KB write = 540 KB | least traffic; needs orientation-aware rect |

  The "skip the floor in the copy" path is the bandwidth
  optimum because the read side of the floor area is genuinely
  wasted work. The Tanmatsu's display is mounted rotated 270°
  (`PAX_O_ROT_CW`), so the above-horizon *logical* rectangle
  is a vertical strip in raw memory — partial-memcpy is
  awkward, but PPA SRM accepts logical-coordinate block configs
  with picture and offset fields, so it handles the rotation
  transparently. Tanmatsu raw dims with the LCD rotated:
  raw_w=480, raw_h=800. Above-horizon raw rect: rx ∈ [223, 479]
  (257 columns), ry ∈ [0, 800).

  **Parallelism**: PPA is a separate hardware engine. With
  `PPA_TRANS_MODE_NON_BLOCKING`, the CPU returns from
  `ppa_do_scale_rotate_mirror` immediately and can paint the
  floor while the PPA DMAs the sky. The two writers do not
  overlap in the framebuffer, so no write-write race. A binary
  semaphore given from the `on_trans_done` callback is taken
  before `render_obstacles` (obstacles can extend above the
  horizon, so the sky must be in place by then). Expected:
  `bg` total wallclock = max(PPA, CPU floor) ≈ 12–15 ms
  instead of the current 17 + 17 = 34 ms.

  **Cache coherency**: PPA reads/writes PSRAM via DMA,
  bypassing the CPU's L1/L2 cache. Two coherency points to
  handle:
  1. **Stale source** — CPU populates `backdrop_cache` at boot
     via PAX; the writes sit in cache until eventual
     writeback. Solution: one `esp_cache_msync(...,
     ESP_CACHE_MSYNC_FLAG_DIR_C2M)` writeback after the
     backdrop is fully drawn, before the first PPA op.
  2. **Stale destination** — PPA writes the fb's above-horizon
     region directly to PSRAM; CPU cache lines for that region
     may be stale. The CPU does not read the fb except via
     `bsp_display_blit` (which goes through DMA from PSRAM and
     is unaffected). If we ever read fb from CPU, add an
     invalidate on the PPA-written region.

  **Alignment**: PPA requires source and destination pointers
  + sizes aligned to L1 / L2 cache line size (128 B on
  ESP32-P4 PSRAM). The BSP-allocated fb is already
  DMA-capable. The backdrop_cache must switch from
  `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` to
  `heap_caps_aligned_alloc(128, ..., MALLOC_CAP_SPIRAM |
  MALLOC_CAP_DMA)`. Current size 480 × 800 × 2 = 768000 bytes
  is already a multiple of 128, so no padding needed.

  **Why this is the right next step before custom rasterizers**:
  PPA hinges entirely on the screen format being something the
  PPA engine supports natively — RGB565 / RGB888 / ARGB8888 —
  which is why the RGB565 switch had to land first. Now that
  the format is settled, the backdrop pipeline gets the biggest
  remaining single win without touching any drawing code.

  Pending after PPA lands:
  - Direct-565 line rasterizer for obstacle wireframes (the
    47 ms `obs` cost is dominated by `pax_simple_line`'s
    per-pixel function-pointer dispatch over ~1000 lines per
    frame).
  - Tighten the vertical-lane iteration in `synthwave_step`
    (`kx_min..kx_max` currently spans ±half_w_world_at_far ≈
    ±53, producing ~108 line draws when only ~25 have
    meaningful visible length). Defer until after PPA + line
    rasterizer to see what's left.

- 2026-05-11 — **PPA pipeline landed (single-cache version) + bugs.**
  First-pass implementation kicked PPA SRM async at the start of
  the frame and waited for the completion semaphore right before
  the foreground passes. The CPU painted the floor in parallel
  with the SRM copy.

  Initial result: gameplay went from 11.8 FPS (RGB565 alone) to
  **13.6 FPS**. The bg phase dropped from 34.6 ms (17.1 + 17.5)
  to 22.1 ms — `bgwait=0.00` every frame meant PPA always finished
  before the CPU floor work did, so the parallelism was fully
  effective. `obs` was unchanged at 46 ms (unsurprising — PPA
  doesn't help with the per-line CPU work in `render_obstacles`).

  Boot quirk along the way: the first attempt allocated
  `backdrop_cache` with
  `heap_caps_aligned_alloc(128, …, MALLOC_CAP_SPIRAM |
  MALLOC_CAP_DMA)`. On ESP32-P4 that combination is risky —
  `MALLOC_CAP_DMA` historically maps to AHB-DMA-able internal
  SRAM and doesn't combine cleanly with `MALLOC_CAP_SPIRAM`. The
  app deadlocked at load time inside `mspi_timing_psram_tuning`
  + `esp_pm/pm_impl.c` (Core 0 spinlock vs. Core 1 IPC, IWDT
  fired). Dropping `MALLOC_CAP_DMA` and keeping plain
  `MALLOC_CAP_SPIRAM` fixed it — on P4, PSRAM is AXI-DMA-
  accessible by default and the cap flag isn't needed.

- 2026-05-11 — **Backdrop split into separate layer caches
  (PPA Option A pipeline).** The single-cache PPA backdrop works
  for the current static sun but doesn't accommodate the moving
  sun for Phase 5. Split into two PPA-driven caches with
  per-frame FILL → SRM → BLEND:

  - `sun_cache` (800×180 logical, RGB565): tight bbox of the sun
    bands. Background is the sky purple so the SRM is harmless
    in the gaps. Rendered at boot with `synthwave_draw_sun(…, +4)`
    so the top band lands at cache y=0; Phase 5 will animate the
    SRM destination offset.
  - `mountain_cache` (800×163 logical, RGB565): tight bbox of the
    visible mountain band. Background is a pure-green colour-key
    (`0xFF00FF00` in ARGB, `0x07E0` in 565); mountains, wireframes,
    and the horizon line are painted on top. PPA BLEND uses an
    `fg_ck_rgb_low_thres = (0, 0xFC, 0)` /
    `fg_ck_rgb_high_thres = (0, 0xFF, 0)` window so the key
    catches the 565→888 expansion regardless of whether the
    silicon does shift or replicate.
  - Per-frame: PPA FILL covers the whole sky region with sky
    purple (kills leftover obstacle pixels from the previous
    frame), then PPA SRM lays down the sun, then PPA BLEND
    composites the mountains.

  The split adds a `heap_caps_aligned_alloc(128, …,
  MALLOC_CAP_SPIRAM)` for each cache; both sizes are rounded up
  to the 128-byte L2 line. Each cache is wrapped in its own
  `pax_buf_t` with the same orientation/endianness as the main
  fb. After PAX finishes drawing into each, one
  `esp_cache_msync(C2M | TYPE_DATA)` flushes the cache to PSRAM
  so PPA's DMA reads see the rasterised pixels.

  **Orientation gotcha (caused the "no sun, no mountains, just
  sky" symptom on first run):** `pax_buf_init` takes *raw*
  dimensions, not logical. Under PAX_O_ROT_CW the raw layout is
  the logical layout transposed. Passing
  `pax_buf_init(&sun_cache, …, 800, 180, …)` (logical dims) gave
  PAX an 800-wide × 180-tall raw buffer; after ROT_CW it
  presented as a 180-wide × 800-tall *logical* surface, so all
  the sun bands (x ∈ [294, 506]) clipped out of bounds.
  Fix: pass `pax_buf_init(&sun_cache, …, SUN_CACHE_LOG_H,
  SUN_CACHE_LOG_W, …)` — raw dims = logical dims transposed.
  Same fix on `mountain_cache`.

- 2026-05-11 — **DIY double-buffering.** With the layer caches
  composed correctly, the left edge of the mountains started
  flickering frame-to-frame. The classic symptom of the CPU/PPA
  writing the framebuffer while the LCD's DMA is reading it.
  Added DIY double-buffering — the BSP supports `num_fbs = 2`
  but `bsp_display_blit` already takes a buffer pointer as an
  argument, so we manage the two buffers ourselves:

  - `fb_a_pixels` / `fb_b_pixels`: two
    `heap_caps_aligned_alloc(128, …, MALLOC_CAP_SPIRAM)` PSRAM
    buffers, full LCD-resolution RGB565.
  - `fb_a` / `fb_b`: matching `pax_buf_t` wrappers with the same
    format / orientation / endianness as before.
  - `fb` (pointer): points at the current back buffer. All CPU
    and PPA drawing targets `*fb`. Every `&fb` in the old code
    became `fb` after the refactor (the underlying type is now
    `pax_buf_t *` instead of a value).
  - Per frame: draw into `*fb`, call
    `bsp_display_blit(…, pax_buf_get_pixels(fb))`, take the
    vsync semaphore, then swap `fb`/`fb_front`. The LCD scans
    out whichever buffer was last blitted; the CPU + PPA only
    ever touch the *other* one.

  Cost: +768 KB PSRAM for the second framebuffer. The flicker
  went away on the first install with this change.

- 2026-05-11 — **PPA cross-client ordering: SRM/BLEND racing.**
  After the orientation fix and double-buffering, parts of the
  sun overwrote the mountains in some frames (and the mountain
  band flickered). PPA is a single hardware engine but each *op
  type* (SRM, BLEND, FILL) is registered as its own client; the
  driver does **not** preserve submission order across client
  boundaries. Empirically the BLEND of the mountain cache
  occasionally landed *before* the SRM of the sun cache,
  producing the visual race.

  Fix: explicit `ppa_wait_one()` between every PPA submit, so
  the ops are enforced FIFO. The CPU floor work stays in
  parallel with the BLEND (the last and longest of the three
  ops), so the bg-phase wallclock is
  `FILL + SRM + max(BLEND, CPU_floor) ≈ 2 + 3 + 22 ≈ 27 ms`.
  Penalty vs. the broken-but-parallel previous attempt: ~5 ms.

  One follow-up parked in case we need the 5 ms back:
  - PPA callback chaining: each `on_trans_done` callback submits
    the next op from ISR context. Restores full parallelism but
    needs cross-checking whether `ppa_do_xxx` is ISR-safe.

  *Note: "enlarge sun_cache to cover the full sky region and drop
  FILL" was considered and rejected. Once the sun starts moving
  in Phase 5 the SRM destination range slides with sun_dy, so no
  fixed cache size guarantees the SRM covers every sky pixel —
  and the FILL also has a second job (wiping stale obstacle
  pixels that drifted above the horizon in the previous frame).
  The FILL stays.*

- 2026-05-11 — **Narrowed lane-line range to the playfield.**
  `synthwave_step` was iterating `kx_min..kx_max` derived from
  the far-plane half-width (≈ ±53 world units, ~108 lines per
  frame). The walls live at world-x = ±5, so every line outside
  that band would have been occluded by a wall obstacle anyway.
  Hardcoded `kx_min..kx_max = -5..+5` (11 lines, independent of
  `cam_x`).

  Smaller win than I expected: -3.7 ms on `bgflr` (18.4 → 14.7),
  +0.7 FPS. The reason — the lines we *removed* are the ones at
  large |k − cam_x|, which sweep nearly horizontally and exit
  the screen quickly (short pixel runs). The lines we *kept* are
  the near-vertical ones that span the full floor height (long
  pixel runs). The per-line cost was always weighted toward the
  long ones we still draw.

- 2026-05-11 — **Performance scoreboard end-of-day.**

  | Stage | FPS gameplay | bg total | obs |
  |---|---|---|---|
  | Baseline RGB888 | 9.5 | 54.4 | 47.0 |
  | RGB565 | 11.8 | 34.6 | 46.0 |
  | RGB565 + PPA single SRM | 13.6 | 22.1 | 46.4 |
  | RGB565 + PPA Option A 3-op + DB | 12.5 | 29.0 | 45.9 |
  | + narrowed lane range | **13.2** | **25.4** | **46.5** |

  Net: **+39%** gameplay FPS from the original baseline, with
  correct sun-behind-mountain layering, no tearing, and the
  pipeline ready for the Phase 5 moving sun.

  Frame budget at 60 Hz is 16.67 ms; we're at ~76 ms (13.2 FPS).
  Phase composition of a typical gameplay frame now:
  - `obs = 46 ms` (61%) — `pax_simple_line` × ~1000 wireframe
    lines per frame, per-pixel function-pointer setter
    dispatch. RGB565 didn't help because it's dispatch-bound,
    not pixel-write-bound. **Next target.**
  - `bg total = 25 ms` (33%) — PPA FILL+SRM+BLEND serialised +
    CPU floor in parallel with BLEND.
  - `fgrest + blit + input + phys ≈ 5 ms` (6%).

  Plan: direct-RGB565 Bresenham line rasterizer for the obstacle
  wireframes (same shape as the existing
  `hershey_direct_draw_line` in `hershey_font_direct.h`),
  lifted into a shared helper and called from
  `render.c:render_obstacles` in place of `pax_simple_line`.
  Triangle fills stay on PAX — `pax_range_setter_16bpp` is
  already a tight `memset16` and faces aren't the bottleneck.

- 2026-05-11 — **`magicnumbers.h` + `direct_565.h` shared helpers.**
  Pulled display geometry (`DISPLAY_RAW_W/H`, `DISPLAY_LOG_W/H`,
  `DISPLAY_RAW_STRIDE`) into a single header so the custom
  rasterizers can hardcode rotation, resolution and stride at
  compile time while keeping the numbers portable to other
  displays. Replaced literal `800` / `480` screen-dim references
  in `synthwave.c`, `render.h`, and `main.c`'s cache-dim defines
  with the new constants. Pre-baked artwork coordinates (sun /
  mountain polygon vertex tables) left alone — those are art
  assets, not display dims.

  `direct_565.h` is header-only, all `static inline`:
  - `direct_565_pack` / `direct_565_pack_for` — ARGB8888 → RGB565
    halfword, once per primitive, byte-swap-aware.
  - `direct_565_logical_index(lx, ly)` — ROT_CW logical → raw index.
  - `direct_565_set_pixel(...)` — bounds-checked single pixel.
  - `direct_565_line(...)` — Bresenham carrying a `uint16_t*`
    instead of recomputing the raw index per pixel. Steps are
    `±1` (for ly±1) and `±DISPLAY_RAW_STRIDE` (for lx±1) —
    constant under the hardcoded rotation, no multiplies per
    pixel.

  Also rewrote `hershey_font_direct.h` to use the shared helpers
  — eliminated its own orientation switch and 24bpp branch, and
  removed the `hershey_native_color_t` struct. Hershey now packs
  the color once per *string* and the inner Bresenham is the
  same code path as everything else.

- 2026-05-11 — **Direct-565 line rasterizer — small win.** Wired
  `direct_565_line` into `synthwave_step` (the floor's 11 lane
  lines + ~10 horizontal stripes) and `render_obstacles` (the
  ~720 cube wireframe edges per frame).

  | Phase | Before | After | Δ |
  |---|---|---|---|
  | bgflr | 14.7 | 13.7 | -1.0 ms |
  | obs   | 46.5 | 46.0 | -0.5 ms |
  | FPS   | 13.2 | 13.3 | ~noise |

  Honest result: much smaller than I'd predicted. I'd assumed
  PAX's `pax_simple_line` was paying a per-pixel function-pointer
  setter cost; the numbers say it isn't (or PAX has a specialised
  line path that's already close to a direct halfword store). The
  ~1.5 ms total savings is per-call setup avoided (color
  conversion + getter/setter lookup), not per-pixel cost.

  Important implication for the next step: if pixel writes
  weren't the bottleneck in `obs`, and lines aren't either,
  then the cost has to be in `pax_simple_tri`'s *per-triangle
  setup* — vertex sort, edge slopes, clip-rect intersection,
  range-setter lookup, all done per call. With ~480 triangles
  per frame this dominates the actual fill.

- 2026-05-11 — **Direct-565 triangle rasterizer — the big win.**
  Built on the line work: `direct_565_tri` in `direct_565.h`,
  used by `render_obstacles` for every cube face (front / side /
  top). Color packed once per face (cube has up to 3 colors:
  side, top, front).

  Two design choices that matter:
  1. **Scan in logical-X direction**, not Y. Under PAX_O_ROT_CW
     a "logical horizontal scanline" (constant ly, varying lx)
     maps to a *strided* raw write — each pixel hits a different
     cache line, PSRAM-hostile. A "logical vertical scanline"
     (constant lx, varying ly) maps to a *contiguous* raw byte
     range, so the whole scanline lives in one or two cache
     lines and the inner loop is decrement-pointer + store.
  2. **Zero per-call setup beyond what we genuinely need**: 3
     conditional vertex swaps, 3 edge slopes, then straight into
     the per-column scanline loop. No clip-rect intersection
     (handled by the per-pixel clamping in `direct_565_vrun`),
     no getter/setter dispatch, no `pax_col_t` conversion (color
     is pre-packed by the caller).

  Edge rule: simple `ceilf(yt)` / `floorf(yb)` snapping. Shared
  cube-face edges might occasionally drop or duplicate a 1-px
  row but the cyan wireframe overlay hides any seam. If we ever
  go wireframe-free, a half-space rasteriser with top-left rule
  would fix it.

  | Phase | Before | After | Δ |
  |---|---|---|---|
  | bgkick | 10.61 | 10.61 | — |
  | bgflr  | 13.7  | 14.1  | +0.4 (noise) |
  | obs    | **46.0** | **5.4** | **-40.6 ms (-88%)** |
  | fgrest | 3.9   | 4.1   | ~ |
  | blit   | 0.7   | 0.7   | — |
  | **FPS gameplay** | **13.3** | **28.3** | **+15.0 (+113%)** |

  This single change is the largest perf win of the session —
  confirms `pax_simple_tri` was setup-overhead-bound for the
  small-triangle case. Our custom rasterizer with no per-call
  bookkeeping and cache-friendly scanlines hits ~5 ms for ~480
  triangles per frame.

- 2026-05-11 — **End-of-day final scoreboard.**

  | Stage | FPS gameplay | bg total | obs |
  |---|---|---|---|
  | Baseline RGB888 | 9.5 | 54.4 | 47.0 |
  | RGB565 | 11.8 | 34.6 | 46.0 |
  | RGB565 + PPA single SRM | 13.6 | 22.1 | 46.4 |
  | RGB565 + PPA Option A 3-op + DB | 12.5 | 29.0 | 45.9 |
  | + narrowed lane range | 13.2 | 25.4 | 46.5 |
  | + direct_565 lines | 13.3 | 24.3 | 46.0 |
  | **+ direct_565 triangles** | **28.3** | **24.7** | **5.4** |

  **3× the original baseline.** Frame composition at 28.3 FPS
  (~35 ms/frame):
  - `bgkick + bgflr = 24.7 ms` (70%) — PPA pipeline + CPU floor
  - `obs = 5.4 ms` (15%) — obstacles
  - `fgrest + blit + input + phys ≈ 5 ms` (15%)

  Background pipeline (PPA serialisation + CPU floor work) is
  the dominant cost now. To hit 60 Hz we'd need another ~18 ms,
  most of it from `bg` and `bgflr`. See "Future FPS improvements"
  section below for the parked options.

  Decision: **pause perf work and resume the phase plan**. The
  optimisation headroom from 9.5 → 28 FPS is enough to absorb
  the upcoming Phase 5 (sun + shadow) and Phase 6 (tris +
  multiplier) feature work without dropping below playable
  framerates. If a future feature blows the frame budget,
  revisit the parked options.

- 2026-05-11 — **Phase 5 design: position-coupled sun + shadow
  stall.** The Phase 5 plan in the Module Responsibilities
  section below describes a *timer*-based sun (`sun_seconds_left`
  ticks down at 1.0/s or 1.5/s in shadow). Revised after user
  clarification 2026-05-11: the mechanic is **position-based with
  ship-speed coupling** — there is no separate countdown. The
  sun's vertical position is the run's primary state; how fast it
  sinks depends on ship speed.

  **Sun motion.** At cruise speed the sun travels
  `GAME_SUN_SINK_RANGE_PX = 120` logical pixels from baseline
  (high) to fully behind the mountains in
  `GAME_SUNSET_SECONDS_AT_CRUISE = 70` s. Off-cruise the rate
  scales:

      base_rate       = GAME_SUN_SINK_RANGE_PX / GAME_SUNSET_SECONDS_AT_CRUISE
      speed_influence = base_rate / GAMEPLAY_CRUISE_SPEED
      d(sun_y)/dt     = base_rate - speed_influence * (ship_speed - cruise)

  At cruise → `base_rate`. Slower → faster sinking. Faster (boost)
  → slower sinking, possibly negative (sun *rises*, player catches
  up). All four tuning knobs live in `magicnumbers.h`.

  **Shadow length.** Linear interpolation on sun position:

      sun_norm     = clamp(sun_y / GAME_SUN_SINK_RANGE_PX, 0, 1)
      factor       = lerp(GAME_SHADOW_LEN_FACTOR_MIN,   // 0.5
                          GAME_SHADOW_LEN_FACTOR_MAX,   // 2.0
                          sun_norm)
      shadow_len_z = obstacle.height * factor

  Sun high (start of run) → shadows half the obstacle's height.
  Sun about to set → shadows twice the obstacle's height. Past
  sunset (`sun_y >= GAME_SUN_SINK_RANGE_PX`) the whole world is
  treated as in shadow regardless of geometry.

  **Shadow detection (per frame).** For each active CUBE
  obstacle the ship is shadowed iff:

      obs.z_world > ship.z_world                            // ship behind obstacle
      (obs.z_world - obs.half_d) - shadow_len_z
              < ship.z_world                                // shadow reaches ship
      |obs.x_world - ship.x_world|
              < obs.half_w + ship.half_w                    // lateral overlap

  Plus an unconditional "in shadow" branch when the sun has
  fully set.

  **Shadow stall (the gameplay consequence).** While in shadow
  the ship decelerates *linearly* — from cruise to zero in
  `GAME_SHADOW_STALL_SECONDS = 6` s. The decel rate is
  `cruise / GAME_SHADOW_STALL_SECONDS = 2 u/s²`. Out of shadow,
  the existing speed-recovery dynamics (from the Phase 4 scrape
  recovery) ramp speed back toward cruise. **The run ends only
  when ship speed actually reaches zero** — so a player coasting
  to a halt in full sunset can still grab a boost pickup, recover
  speed, push the sun back up, and survive. Provides a real
  comeback window.

  **The feedback loop.** Slow ship → sun sinks faster → shadows
  longer → ship more often shadowed → slower. Boost ship → sun
  rises → shadows shorter → easier to maintain speed. Same
  shape as the original Race The Sun, but expressed through a
  single integrated state (sun_y) instead of two coupled
  timers.

  **Shadow rendering.** New draw pass between `synthwave_step`
  and `render_obstacles`. Each shadow is a flat trapezoid on
  the y=0 ground plane, world-space rectangle projected through
  the existing `render_project` and drawn as two triangles via
  `direct_565_tri`. Solid darker-purple fill
  (`GAME_SHADOW_FLOOR_COLOR`) — no alpha blending, no
  read-modify-write. Compound overlaps just paint the same
  dark color over each other, which is the correct visual.

  After full sunset the floor base color switches to the
  shadow color (the FILL rect, and any other floor paint, uses
  `GAME_SHADOW_FLOOR_COLOR` instead of the purple base). The
  per-obstacle shadows become uniform across the whole floor
  at that point so painting them individually would be wasted
  work.

  **Tunables added to magicnumbers.h** (2026-05-11):
  `GAMEPLAY_CRUISE_SPEED`, `GAME_SUN_SINK_RANGE_PX`,
  `GAME_SUNSET_SECONDS_AT_CRUISE`, `GAME_SHADOW_STALL_SECONDS`,
  `GAME_SHADOW_LEN_FACTOR_MIN/MAX`, `GAME_STAGE_SECONDS`,
  `GAME_REST_SECONDS`. Derived: `GAME_STAGE_LENGTH_Z`,
  `GAME_REST_LENGTH_Z`. `SHIP_BASE_SPEED_Z` and
  `WORLD_STAGE_LENGTH_Z` / `WORLD_REST_LENGTH_Z` now derive
  from these to keep distance and time in sync.

  **What this supersedes:** the timer-based "Sun mechanic &
  shadow" bullet in the `game.c` module description below. The
  position-coupled model fits the "chase the sun" theme more
  naturally and removes the need for a separate
  `sun_seconds_left` countdown — sun position *is* the timer.

- 2026-05-11 — **Phase 5 first pass landed (sun + shadows + stall).**
  All the sun/shadow geometry and physics from the revised design
  is on-device:

  - `game.sun_y` integrated each frame from ship speed
    differential vs cruise. Clamped to `[0, GAME_SUN_SINK_RANGE_PX]`.
  - PPA SRM destination Y wired to `(int)game.sun_y`; sun visibly
    sinks during a run, the FILL handles the newly-exposed sky.
  - Per-cube shadow detection via `is_ship_in_shadow` logic inside
    `game_after_collide`. CUBE obstacles only; walls / pickups
    skipped.
  - Shadow quads on the floor via the new `render_shadows`
    function — flat trapezoids on the y=0 plane using
    `direct_565_tri`, color = `GAME_SHADOW_FLOOR_COLOR`.
  - Floor paint split into `synthwave_step_base` (rect) and
    `synthwave_step_lines` (lane lines + stripes) so shadow
    quads can paint between them; lane lines stay bright magenta
    on top of shadows with no per-pixel blend math.
  - Post-sunset state: `synthwave_step_base` paints the whole
    floor with the shadow colour instead of the regular purple,
    `render_shadows` early-outs, and the ship is treated as
    permanently in_shadow.
  - Shadow stall: ship decelerates linearly to zero in
    `GAME_SHADOW_STALL_SECONDS` while in_shadow. Game-over fires
    when speed reaches 0, same end-of-run code path as a head-on
    collision.
  - Debug controls: Q nudges `sun_y` toward sunset, A nudges back
    toward zenith (10 px per press). Live `sun=NNN` readout under
    the speed `v=NNN` readout. Also removed the dead WASD steering
    bindings while wiring up the new scancodes.

  **Tuning pass after on-device playtesting (2026-05-11):**

  | Constant | Initial | Final | Reason |
  |---|---|---|---|
  | `GAME_SUN_SINK_RANGE_PX` | 120 | **160** | parts of the sun still peeked through mountain valleys at 120; 160 (found visually with the Q/A nudge) fully hides every band |
  | `GAME_SHADOW_LEN_FACTOR_MIN` | 0.5 | **1.0** | shadows too short at the start of a run; 1.0 = "shadow equals obstacle height" reads as actual shadow geometry |
  | `GAME_SHADOW_LEN_FACTOR_MAX` | 2.0 | **6.0** | shadows too short near sunset; 6.0 produces dramatic long bars across the floor as the sun lowers |
  | `GAME_SHADOW_STALL_SECONDS` | 6.0 | **8.0** | 6 s wasn't enough recovery window for the upcoming boost pickup; 8 s gives more time to grab a boost |
  | `GAMEPLAY_CRUISE_SPEED` | 12 | **20** | wanted a more frenetic pace; world-z stage budgets auto-scale to keep stage seconds constant |
  | `GAME_SUN_SPEED_INFLUENCE` | (hard-coded 1.0) | **3.0** | promoted to tunable; 3.0 makes 2× cruise (40 u/s) produce a sun *rising* at 2× base rate — boost will visibly catch up the sun |

  **Newly added tunables in magicnumbers.h:**
  - `GAME_SUN_SPEED_INFLUENCE` — sensitivity of sun rate to
    speed deviation from cruise. Default 3.0. Freeze speed
    formula: `cruise × (1 + 1/INFLUENCE)`.
  - `GAME_SHADOW_FLOOR_COLOR` — solid dark purple painted under
    obstacle shadows and across the whole floor post-sunset.

  **Still TODO in Phase 5** (in suggested order):
  1. Boost pickups — spawn `OBSTACLE_KIND_PICKUP_BOOST` in rest
     areas and sparingly in obstacle areas. Collection sets
     `ship_speed_z` to a tunable boost target (above the sun's
     freeze speed of ~27 u/s) for a tunable duration. Without
     this, every run ends in sunset — there's no recovery loop.
  2. Boost-active HUD indicator (small).
  3. Run-end polish (fade as ship_speed → 0, or just live with
     the current freeze).

- 2026-05-11 — **Ship shadow visual feedback partially landed.**
  - Implemented: ship sprite tints 30% darker when
    `game.in_shadow` (tunable `GAME_SHIP_SHADOW_TINT`). Dim
    happens once per frame on the four ship colours; per-pixel
    cost is zero.
  - **Not implemented (tried and reverted):** rendering the
    ship's own shadow on the floor. The ship sits very close to
    the camera (`SHIP_Z_PLANE = 2.0`, ship near-face at z≈1.66)
    so the projected shadow lands at the very bottom of the
    screen — almost entirely below the visible floor area — and
    isn't readable. Removed the `render_ship_shadow` function,
    the call site, and `GAME_SHIP_SHADOW_HEIGHT`. Per-obstacle
    shadows on the floor + the ship sprite tint together
    provide enough "you are in a shadow now" signal.

- 2026-05-12 — **Speed-booster pickups landed — Phase 5 closes.**
  The recovery loop is now playable: collect a pyramid, ship
  briefly surges to a sun-rising speed, sun climbs back, and
  shadow stalls become survivable. Phase 5 row in the status
  table flipped to ✅.

  **Boost state machine** in `game.h/c`:

      IDLE → RAMPING (on pickup)
      RAMPING → HOLDING after GAME_BOOST_RAMP_UP_SECONDS
      HOLDING → COASTING after GAME_BOOST_HOLD_SECONDS
      COASTING → IDLE when ship_speed_z ≤ base_speed_z

  `game_state_t` gains `boost_phase`, `boost_phase_time`, and
  `boost_ramp_start_speed`. The phase machine is checked at the
  *top* of `game_after_collide`'s speed-dynamics block with this
  priority order:

  1. **RAMPING / HOLDING** override everything — the ship's
     speed is forced to the lerp/peg value. Shadow stalls,
     scrape decel, etc. all yield. (This is the whole point of
     the boost: escape stalls.)
  2. **Shadow stall** wins over **COASTING** so a player who
     wanders back into shadow during the coast still pays the
     stall cost.
  3. **COASTING** linear decel at `GAME_BOOST_COAST_DECEL` until
     `ship_speed_z ≤ base_speed_z`, then drops to IDLE.
  4. **Normal scrape / recovery** dynamics (unchanged from Phase
     4) — only active when boost is IDLE and ship isn't in
     shadow.

  Picking up another booster mid-boost just resets to RAMPING
  with the current speed as the lerp start (so picking up at
  near-peak speed extends the hold rather than stuttering).

  **Collision plumbing.** `game_collide` signature changed from
  `world_state_t const*` to `world_state_t*` so the booster
  case can deactivate the obstacle on contact. The
  `OBSTACLE_KIND_PICKUP_BOOST` switch arm:

  ```
  o->active                 = false;
  g->boost_phase            = BOOST_RAMPING;
  g->boost_phase_time       = GAME_BOOST_RAMP_UP_SECONDS;
  g->boost_ramp_start_speed = g->ship_speed_z;
  skip_response             = true;   // no scrape, no head-on
  ```

  Skip-response keeps the booster from triggering the wall /
  cube collision paths.

  **World scheduling.** `world_state_t` gains
  `booster_due_at_progress[GAME_BOOSTERS_PER_STAGE]`. On
  `start_stage` these are filled with stage-progress positions
  obtained by dividing the stage length into N equal segments
  and placing one booster in each segment at a jittered fraction
  in [0.25, 0.75] (deterministic from the stage PRNG). On each
  `world_advance` the booster scheduler checks whether the new
  `stage_progress = WORLD_STAGE_LENGTH_Z - stage_z_remaining`
  has crossed any due-at-progress; if so it calls
  `spawn_booster` (which places one at the far-plane spawn z)
  and marks that slot with the `-1` sentinel.

  The rest area spawns its `GAME_BOOSTERS_PER_REST` boosters
  once on rest-area entry (in the `area_init_rest` call site in
  `world_advance`, not inside the init function — keeps
  `area_init_rest` a pure state initialiser).

  **Pyramid rendering.** `render.c` dispatches on
  `obstacle_kind_t` inside the painter's-sort loop. Cubes /
  walls take the existing cube path. `OBSTACLE_KIND_PICKUP_BOOST`
  goes through a new `render_booster_pyramid` helper that:
  - Projects 5 vertices (apex at y = height, 4 base corners at
    y = 0).
  - Skips the back face (camera always in front).
  - Draws one side face (whichever's facing the camera) then
    the front face with `direct_565_tri`.
  - Wireframes the visible apex-edges and base-edges with
    `direct_565_line`.
  - Modulates the fill colour by a per-frame `pulse_factor`
    computed once from `esp_timer_get_time()`:
    `1.0 - PULSE_AMPLITUDE × 0.5 × (1 - sin(2π × phase))`.
    Outline stays at full brightness so the silhouette doesn't
    breathe with the body.

  **HUD indicator.** Bottom-left of the screen, a solid green
  upward-pointing triangle whose height is 3× the 18 px debug
  text size (so the player can read it out of the corner of
  their eye). Visible whenever `boost_phase != BOOST_IDLE` —
  covers the full RAMP / HOLD / COAST window. Drawn via
  `direct_565_tri` in PLAYING state only (GAME_OVER and TITLE
  don't show it).

  **Tunables added to magicnumbers.h:**
  - `GAME_BOOST_TARGET_SPEED = 40.0` — peg speed during HOLD.
    Re-derive when cruise / influence change; the formula in
    the file's comments is
    `ship = cruise × (1 + 3/INFLUENCE)` for "sun rising at 2×
    base rate".
  - `GAME_BOOST_RAMP_UP_SECONDS = 2.0`
  - `GAME_BOOST_HOLD_SECONDS = 1.0`
  - `GAME_BOOST_COAST_DECEL = 2.0` — slower than scrape decel
    (5.0) so a boost gives a long tail of above-cruise travel.
  - `GAME_BOOSTERS_PER_STAGE = 4`
  - `GAME_BOOSTERS_PER_REST = 1`
  - `GAME_BOOSTER_HALF_W = 0.4`,
    `GAME_BOOSTER_HEIGHT = 2 × HALF_W = 0.8` (height equals
    base side as requested).
  - `GAME_BOOSTER_FRONT_COLOR`, `GAME_BOOSTER_SIDE_COLOR`,
    `GAME_BOOSTER_OUTLINE_COLOR` — neon green palette.
  - `GAME_BOOSTER_PULSE_PERIOD_S = 1.2`,
    `GAME_BOOSTER_PULSE_AMPLITUDE = 0.3` — body breathes ±30%
    brightness over 1.2 s.

  **What's left for Phase 5 polish (deferred):**
  - Run-end fade as `ship_speed → 0`. The current behaviour
    (instant freeze + GAME OVER overlay) is functional; a
    couple seconds of slow-mo would feel better but isn't
    strictly necessary.
  - Per-phase visual differentiation on the HUD indicator (e.g.
    pulsing during RAMP, steady during HOLD, dimming during
    COAST). Steady-on-all-phases is readable enough.

- 2026-05-12 — **Persistence redesign: file-based NBT saves with
  three independent slots.** NVS dropped as the persistence
  store. New layout:
  - **Path**: `/int/synthracer/save{0,1,2}.bin`. The `/int`
    mount is already used for icon PNGs, no new mount needed.
    `save_init()` creates the directory at boot.
  - **Format**: NBT, copied verbatim from paperclips
    (`game_nbt.c`/`game_save.c`). Magic changed to **`SYNT`**,
    format version starts at **1**. Same endian sentinel and
    tag types (END/INT32/INT64/DOUBLE/STRING/COMPOUND).
  - **Tools**: `tools/savetool.pl` mirrors the paperclips
    helper — `decompile <bin> <txt>` and `compile <txt> <bin>`
    so saves are inspectable and we can construct test
    scenarios from text files (e.g. "slot with all unlocks
    + 7 stages reached" for testing the stats screen).
  - **Three slots, no autosave slot.** Slot 0/1/2 are
    independent profiles (stats + unlocks + daily-progress per
    slot). When the player picks a slot at boot, that slot is
    sticky for the app lifetime; we autosave to it after every
    run (no separate autosave file).
  - **Explicit-boolean fields**, not bitmasks. Each unlock
    has its own INT32 (0 or 1): `unlock_speed_boost`,
    `unlock_multiplier`, `unlock_jump`, `unlock_magnet`,
    `unlock_shield`, … 25 entries total. Same pattern for
    today's three daily-challenge slots:
    `daily_done_1pt`, `daily_done_2pt`, `daily_done_3pt`. NBT
    has no native bool tag — we use INT32 0/1 with a leading
    `b_` naming hint isn't necessary because the field names
    are self-describing. Easy to grep, easy to edit by hand
    via the savetool round-trip.

- 2026-05-12 — **Stats tracking (per slot).** The save's
  `stats` compound records two parallel sets of counters:
  - **Last run**: `score_last` (i64), `distance_last` (f64,
    world units), `stage_last` (i32), `multiplier_last_max`
    (i32), `run_end_reason` (i32: 0=in-progress, 1=crash,
    2=stall, 3=sunset, 4=quit).
  - **All-time bests / totals**: `score_best` (i64),
    `distance_total` (f64), `stage_best` (i32),
    `multiplier_best` (i32), `runs_total` (i32),
    `runs_crashed` (i32), `runs_stalled` (i32),
    `runs_sunset` (i32), `runs_quit` (i32),
    `play_time_total_s` (f64).
  Counters are committed to the save on run end (by
  `save_commit_run_end(...)`), which writes the file
  immediately. No async autosave thread today — the run is
  paused at the GAME OVER screen anyway, so a synchronous
  write is fine.

- 2026-05-12 — **Stats compound refactored to symmetric
  `run_stats_t` (twin compounds `last_run` and `all_time`).**
  Followup on the persistence redesign: instead of one flat
  compound with asymmetric `_last` / `_best` / `_total` pairs,
  the on-disk `stats` compound contains two children of the
  *same* schema:
  ```
  stats {
    last_run { score=…, distance=…, stage_reached=…, ... }
    all_time { score=…, distance=…, stage_reached=…, ... }
  }
  ```
  Both are `run_stats_t` from `save.h`. The merge rule
  (`run_stats_merge_into_all_time` in save.c) is the single
  place that knows max-tracked fields (score, stage_reached,
  multiplier_max) versus sum-tracked fields (distance,
  duration_s, pickup counters, runs_total + the four
  end-reason flag-counters). Same field name in both
  compounds means the stats screen renders both with a single
  helper (`draw_stats_block(y, label, run_stats_t const*)`),
  and constructing test scenarios in the savetool text file
  is symmetric.

  Pickup counters live on `run_stats_t` from day one:
  `pickups_speed_boost` (wired), `pickups_tri`, `pickups_jump`,
  `pickups_shield` (placeholders, increment when the
  corresponding pickup types land in Phases 6 / 9).
  `game_state_t` gained matching per-run counters that the
  commit copies into `last_run.pickups_*` on run end.

- 2026-05-12 — **F4 pause menu (Resume / Abort run); F1 is a
  no-save dev-only exit.** The pause overlay is `STATE_PAUSED`:
  - F4 in `PLAYING` → `PAUSED`. F4 in `PAUSED` → resume.
  - `PAUSED` cursor moves with UP/DOWN, ENTER picks:
    - **Resume** → back to `PLAYING`.
    - **Abort run** → `save_commit_run_end(…, SAVE_END_QUIT, …)`
      then `STATE_MENU`. This is the *only* path that records
      a QUIT run.
  - F1 still restarts to launcher but explicitly does **not**
    commit the in-flight run. Losing mid-run progress on F1
    is deliberate — F1 is a development-only escape hatch,
    expected to be removed in the final build. Players who
    want to abandon a run with stats recorded use F4 → Abort.

- 2026-05-12 — **Title screen redesigned as a menu.** New app
  states:
  - `STATE_SLOT_SELECT` — first state on boot. Three rows
    (slot 0/1/2), each labelled with summary stats (best
    score, runs played, last-played date) or `[new]` if the
    file is absent. UP/DOWN to select, ENTER/SPACE/A to
    confirm. F1 exits the app.
  - `STATE_MENU` — main menu with five entries: **Daily Run**
    / **Seeded Run** / **Upgrade Ship** (stub) / **Stats** /
    **Exit**.
  - `STATE_SEED_INPUT` — keyboard ASCII digits entry, max 10
    digits to fit a `uint32_t`. Prefilled from
    `last_custom_seed` in the active save. ENTER starts the
    run; ESC cancels back to menu. BACKSPACE edits.
  - `STATE_STATS_VIEW` — text dump of the stats compound. ESC
    or ENTER returns to menu.
  - `STATE_UPGRADE_STUB` — placeholder "coming soon" screen.
    Slot for future Phase 11 ship-upgrade UI.
  - `STATE_PLAYING` → `STATE_GAME_OVER` → `STATE_MENU` (no
    longer back to a press-space-to-start title screen).
  Custom-seed runs (`STATE_SEED_INPUT` → PLAYING) award
  meta-progression identically to daily runs — see the
  2026-05-15 decisions-log entry. The seed value is stored on
  the save as `last_custom_seed` only to prefill the input
  screen next time.

- 2026-05-13 — **World generation modularised.** The previous
  monolithic `world.c` was carrying object spawners, area
  generators, the obstacle pool, and the orchestrator. Split
  along functional lines:
  - `main/obstacle.{c,h}` — `obstacle_t` + `obstacle_spawn` /
    `obstacle_despawn` helpers + 5 optional callback typedefs
    (`physics`, `cleanup`, `collide`, `draw`, `shadow`). Each
    is NULL-guarded at the call site, falling through to the
    centralised kind-dispatch default. A 128-byte 16-aligned
    `scratch` block lives on each pool entry — objects overlay
    their own state struct via `(my_state_t*)o->scratch`. No
    heap allocation during gameplay; per-object state is
    bounded by the inline buffer. Objects needing more can
    store a heap pointer in scratch and free it in `cleanup`.
  - `main/objects/{cube,wall,booster,bridge}.{c,h}` — one
    file per object type, holding its spawners, palette,
    dimensions, and any custom callbacks it installs.
  - `main/areas/{pixel_field,big_blocks,gateways,bridges,rest}.{c,h}`
    — one file per area generator, importing whichever object
    spawners it composes. Areas no longer touch the
    palettes/dimensions of the objects they use.
  - `main/world.c` — slim orchestrator: PRNG helpers, area
    picker, stage state machine, pool sweep with physics
    dispatch, booster scheduler. About 230 lines (was ~600).
  - `WORLD_OBSTACLE_POOL_SIZE` bumped 128 → **512** so compound
    objects (bridges = 3 entries) can coexist with the always-on
    wall segments (~66) without crowding. BSS grew 26 KB → 100 KB
    in internal SRAM; plenty of headroom on the Tanmatsu's 768 KB.
  - Render and collision still kind-dispatch by default
    (`render_obstacles`, `render_shadows`, `game_collide`) so
    every existing object works unchanged; per-object
    callbacks override when set.

- 2026-05-13 — **Bridges area + concrete-grey bridge object.**
  New visual area type: 1..5 concrete bridges spanning the
  playfield. Stage-agnostic (no difficulty scaling).
  Implementation pattern:
  - `objects/bridge.c` spawns 3 pool entries per bridge: left
    pillar, right pillar, horizontal span.
  - Pillars are cube-kind, sit on top of the side walls (see
    the y_base entry below). Default cube render handles them.
  - Span is cube-kind with three custom callbacks:
    - `draw` — renders the elevated slab (camera below the
      span, so visible faces are bottom + front + wireframe).
    - `shadow` — projects a wide rectangle on the ground.
    - `collide` — returns `OBSTACLE_HIT_IGNORE` so the ship's
      x-z AABB overlap doesn't kill it (ship at y≈0.22 < span
      at y≥3.67 — they physically never touch).
  - `areas/bridges.c` snaps bridge spawn z to the nearest
    wall-segment centre at or below `WORLD_Z_FAR_SPAWN`, so
    each bridge sits exactly on one wall segment and one floor
    stripe. Gap = `2 × BRIDGE_DEPTH = 6 u` (two wall segments)
    so the projected shadow on the floor at sun-up leaves a
    clear lit strip between consecutive bridges — sky pattern
    bridge-open-bridge-open maps to floor pattern
    shadow-lit-shadow-lit.
  - The booster scheduler still increments `boosters_owed`
    during bridge areas; the area's tick fires
    `booster_spawn(w)` at random x on the floor, anywhere in
    the run, when one is owed.

- 2026-05-13 — **`y_base` on `obstacle_t` for elevated cubes.**
  Added `float y_base;` to the obstacle struct, default 0
  (set by `memset` in `obstacle_spawn`). The default cube
  renderer now uses `y_base` for the cube's bottom face and
  `y_base + height` for the top — ground-level cubes are
  unaffected (y_base = 0 → unchanged math). Two consumers
  today:
  - **Bridge pillars** set `y_base = WALL_HEIGHT` so they sit
    cleanly on top of the side walls instead of overlapping
    them (the old "pillar from y=0 with height=3" geometry
    intersected the wall's y=0..0.67 range and produced
    draw-order flicker).
  - **Bridge span** sets `BRIDGE_SPAN_Y_BASE = WALL_HEIGHT +
    BRIDGE_PILLAR_HEIGHT` so it sits on top of the pillars.
    The span keeps its custom draw because the default
    renderer assumes camera-above-cube and doesn't have a
    bottom-face code path (camera at y=1 is below the entire
    span at y=3.67..4.17).

  The default shadow path still uses just `height * factor`
  for shadow length, not `(y_base + height) * factor` — that's
  intentional, kept for ground-cube correctness. Elevated
  shadows that care (the span) install custom shadow callbacks
  that compute the right elevation-based length themselves.

- 2026-05-13 — **TAB debug key forces the next area type.**
  Scancode `BSP_INPUT_SCANCODE_TAB` raises `s_force_area` in
  `input.c`. Main loop consumes it during `STATE_PLAYING` and
  calls `world_force_next_area(&world, AREA_TYPE_BRIDGES)`,
  which zeros the active area's `length_remaining_z` and
  stashes the forced type in `world.forced_next_area_type`.
  The next `start_next_area` call reads the field, bypasses
  `pick_area_type` entirely, and consumes the override. Skips
  min/max-stage gating — that's the point: forcing into bridges
  at stage 1 works the same way it would at stage 50. Change
  the area-type literal in `main.c` to test a different area.

- 2026-05-13 — **Area picker retry loop + min/max stage
  bounds.** `pick_area_type` now loops up to 32 attempts on
  rejection — `area_is_applicable(type, stage)` checks
  `min_stage <= stage <= max_stage`, falls back to
  `AREA_TYPE_PIXEL_FIELD` (always unlocked at stage 1) if no
  candidate fits. All current areas use `[1, 0xFFFF]` so the
  loop never actually re-rolls; the contract is here for
  Phase 10's area gating. The retry consumes PRNG draws per
  attempt, so adding a gated area shifts the per-stage PRNG
  stream for runs that hit the rejection — daily seed
  determinism within a version preserved; cross-version
  stability of "what seed X plays like" not.

- 2026-05-13 — **Stage counter bumped uint8_t → uint16_t.**
  Real-game Race The Sun runs can pass stage 255; the cap was
  hit in YouTube videos. Now stage is `uint16_t` everywhere
  (`world_state_t.stage`, `start_stage`, `mix_stage_seed`,
  `world_lerp_by_stage`, `world_stage_interval_scale`,
  `area_is_applicable`, `pick_area_type`, all `area_X_init`
  signatures). Sentinel for "never expires" is now `0xFFFF`
  (~45 days of continuous cruise to reach). The save struct's
  `stage_reached` was already int32, no save-format change.

- 2026-05-13 — **Pixel-sample shadow detection.** Replaced
  `game_after_collide`'s 512-entry obstacle loop with a single
  framebuffer read. After `render_shadows` paints all the
  shadow quads on the floor (including custom-callback
  shadows like the bridge span) and *before*
  `synthwave_step_lines` overlays lane lines, the floor pixel
  under the ship's foot is either the floor-base 565 (0x5851)
  or the shadow 565 (0x284A). A single `uint16_t ==`
  comparison sets `game.in_shadow` for the next frame.
  Benefits:
  - **Automatic respect for custom shadow callbacks.** No
    parallel math: the painted shadow IS the gameplay shadow.
  - **Cheaper** — one pixel read + one projection per frame,
    replacing a per-entry loop over 512 pool slots.
  - **Compositional** — overlapping shadows from multiple
    objects produce "in shadow" naturally; no special-casing.

  Sharp edges:
  - One-frame stale (sample feeds next frame's physics). At
    cruise (20 u/s) that's 0.33 u of travel — well below a
    half-cube width.
  - The ship's foot at z=`SHIP_COLLISION_Z_C`=1.98 projects to
    sy≈483, just below the 480-pixel floor. `ly` is clamped
    to `DISPLAY_LOG_H - 1`, sampling at z≈2.02 — 0.04 u in
    front of the ship, on the floor.
  - The lane-line collision is avoided by sampling *between*
    shadows and lines: the centre lane line at world x = cam_x
    projects to the ship's screen x, so a post-lines sample
    would hit it.
  - Post-sunset is handled synchronously in `game_after_collide`
    (the floor base is the shadow colour anyway, so the
    sampler would agree — but the sync override beats the
    one-frame lag at the sunset transition).
  - 565-quantisation coupling: if the floor base ever shifts
    to within ~7 quantisation steps in R or B of the shadow
    constant, the sampler silently reads "always in shadow".
    Comment block on `GAME_SHADOW_FLOOR_COLOR` in
    magicnumbers.h flags this.

- 2026-05-13 — **Misc tunings.**
  - **Turn rate scales with forward speed.** Lateral motion
    was `bank * SHIP_TURN_RATE * dt` (flat 3.5 u/s). At boost
    speed the same lateral over 2× longitudinal felt like
    understeer. Now `turn_rate = SHIP_TURN_RATE *
    (ship_speed_z / GAMEPLAY_CRUISE_SPEED)` so the
    lateral-to-longitudinal ratio is constant at any speed.
    Stall turning drops toward zero — physically right, adds
    tension to shadow stalls.
  - **Gateway pad fixed at 50 u.** Used to lerp 30 → 10 over
    stages 1-10, which made hard-left → hard-right alignment
    sequences physically unreachable at cruise speed. The
    entire difficulty curve now lives in the lateral opening
    width; pad is constant.
  - **Booster placement delegated to areas.** `boosters_owed`
    counter on `area_state_t` is bumped by the top-level
    scheduler when a stage-progress mark is crossed; each
    area's tick consumes the counter and decides *where*:
    pixel-field / big-blocks displace a cube spawn, gateways
    drop one in the gap of the next gate, rest dumps any
    leftovers on entry.

- 2026-05-13 — **Stage banner + pre-stage-1 rest area.**
  Rest areas (the breathers between obstacle stages, plus the
  new run-start rest) now show a big green "Stage: N" banner
  top-centre, sized to the text with a translucent dim panel
  behind it (same `direct_565_dim_rect` as the menu panels).
  N is the *upcoming* stage — during the rest that leads into
  stage N+1, `world.stage` is still N (the rollover happens
  when rest ends), so we display `world.stage + 1`. World
  init now sets `world.stage = 0` and initialises an
  `AREA_TYPE_REST` area *before* the first stage; when that
  rest ends, the existing area-done handler fires
  `start_stage(0 + 1) = start_stage(1)` — no special-casing
  in the transition logic. The pre-stage-1 rest still spawns
  its `GAME_BOOSTERS_PER_REST` quota so the player gets a
  greeting boost before any obstacles arrive.

- 2026-05-13 — **HUD additions: F4 pause hint + stage line.**
  Left side gained a second hint slot below "F1 to exit":
  `draw_pause_hint()` mirrors the F1 pattern with the F4 icon
  and "to pause" text. Drawn only during PLAYING — PAUSED
  hides it because the pause overlay's footer already says
  "F4 to resume", and menus / slot-select / game-over have no
  pause concept so the hint would mislead.

  Right side gained a green "Stage: N" line at slot 1
  (between `score=` and `v=`). Same upcoming-stage rule as
  the banner — banner and HUD always agree on the displayed
  stage. Drawn during PLAYING / PAUSED / GAME_OVER so the
  context is preserved through the freeze.

- 2026-05-13 — **Obstacle wall-clearance.** Cube and booster
  spawners used to distribute x over the full track
  `[-TRACK_HALF_WIDTH, TRACK_HALF_WIDTH]`, so an obstacle at
  the extreme would have its outer face poking through the
  side wall by `half_w`. Now `cube_spawn_pixel`,
  `cube_spawn_big`, and `booster_spawn` all clamp the
  random-x range to `±(TRACK_HALF_WIDTH - half_w)`:
  - Pixel cubes: ±4.6 (half_w 0.4)
  - Big blocks: ±4.2 (half_w 0.8)
  - Boosters: ±4.6 (half_w 0.4)
  `booster_spawn_at` (used by the gateway gap-booster path)
  is unchanged — it takes explicit coords that the gateway
  generator already constrains.

- 2026-05-13 — **Flipping cube object + dynamic passage area.**
  New `main/objects/flipping_cube.{h,c}` adds a big-block-sized
  obstacle (same dimensions as `cube_spawn_big`: 1.6 × 1.6 × 2.0)
  that rolls 90° around one of its bottom z-running edges as it
  approaches the camera. Roll progress is a pure function of
  z position — `t = (START_Z - z) / (START_Z - END_Z)` clamped
  to [0,1] with START_Z = 50 and END_Z = 8 — so the animation is
  deterministic, frame-rate independent, and unaffected by speed
  changes mid-flight. Two visual subtypes share the
  implementation: `direction = -1` pivots on the bottom-left
  edge (cube tips LEFT, red outline) and `direction = +1` pivots
  on the bottom-right edge (cube tips RIGHT, green outline).
  Body fill is a blue palette (front 0xFF4080FF / side 0xFF2050B0
  / top 0xFF80B0FF) for both subtypes; the direction is read
  entirely from the wireframe colour.

  Per-object state lives in the 128-byte scratch buffer
  (`flipping_state_t`: direction, progress, x_initial). The
  physics callback runs every frame *before* collision and
  refreshes the obstacle's `x_world`, `half_w`, and `height` to
  the AABB of the currently-rotated cross-section. This means
  `game_collide`'s standard AABB test sees the rolled footprint
  with no extra plumbing, and the default cube shadow renderer
  also tracks the roll automatically (it reads those same three
  fields). `y_base` stays 0 because the pivot stays on the
  ground throughout the roll. The renderer projects all 8
  corners of the rotated body (4 cross-section × 2 z faces) and
  uses face-normal · camera-direction to pick visible side
  faces; the front-face quad is always drawn when not
  near-clipped. Geometry math: a corner at offset (dx, dy) from
  the pivot becomes `(dx·c - dy·s, dx·s + dy·c)` where the
  rotation angle is `-direction · t · π/2` — left-roll = CCW,
  right-roll = CW. Since CUBE_BIG is taller than it is wide
  (height 2.0 vs half-width 0.8), a fully landed cube ends up
  2.0 u wide in the roll direction and 1.6 u tall (i.e.
  shorter and wider than upright).

  New `main/areas/dynamic_passage.{h,c}` is a stage-2..5 area
  with two mirrored layouts. The top-level area picker sees it
  as one slot; `area_dynamic_passage_init` then rolls one extra
  bit from the stage PRNG to pick subtype:
  - Non-mirrored: 4..7 left-rolling cubes (red, dir=-1) spawn
    pressed against the **right** wall. As they near the
    camera they tip LEFT, off the wall and into the
    just-left-of-cubes corridor (the "bait" lane that looked
    safe before the flips). The actual safe lane runs along
    the right wall, exposed once the cubes have left it.
  - Mirrored: same idea swapped — right-rolling green cubes
    against the left wall, flip right, safe lane along the
    left wall.

  Two concurrent emission streams run inside the area:
  flipping cubes at `next_event_z` spacing of `2 × HALF_D`
  (centre-to-centre = 2 × depth = 3.2 u; user spec: gap
  between them equals their depth), and pixel-field clutter
  at `clutter_event_z` cadence of 3.0 u with one-wall-side
  rejection (clutter x rejected if it would land in the
  wall-adjacent safe lane, so the player sees a continuous
  pixel-field wall on the bait side). The clutter timer is
  separate from `next_event_z` so the two streams overlap
  without interference. If the booster scheduler flags a
  booster owed during the area, it spawns in the safe lane
  half a `PASSAGE_FLIP_SPACING` behind the just-spawned
  flipping cube — dead-centre in the z-gap between successive
  cubes once both land. Length budget = `n × spacing` so all
  flipping cubes spawn before the area ends; clutter continues
  filling the tail until the budget runs out.

  Two new fields landed on `area_state_t` to support this:
  `int passage_mirror` (0/1, set at init from the PRNG coin)
  and `float clutter_event_z` (independent clutter cadence
  timer). Both are inert for other area types — the struct
  costs grew by 8 bytes, which is negligible against the
  `WORLD_OBSTACLE_POOL_SIZE × sizeof(obstacle_t)` line.

  Picker / applicability wiring: `AREA_TYPE_DYNAMIC_PASSAGE`
  added to the enum, the picker candidates list, the
  `start_next_area` dispatch, and `area_tick`. Min stage 2 /
  max stage 5 in `area_is_applicable` — this is the first
  area with a non-trivial gate, so the picker retry loop now
  earns its keep (stage 1 + 6+ runs will reject this candidate
  and re-roll). TAB debug-force target retargeted from
  `AREA_TYPE_BRIDGES` to `AREA_TYPE_DYNAMIC_PASSAGE` so the
  new area is the default smoke-test path.

  Design note on the "non-mirrored" wall placement: the user
  spec described "left-rolling cubes spawn alongside the right
  wall" alongside "cubes flip to the right, revealing passage
  close to left wall", which is self-contradictory (a cube
  pressed against the right wall can't tip into it). We
  resolved this in conversation to: cubes are pressed against
  the wall on the OPPOSITE side from their roll direction, and
  the revealed safe lane runs along that SAME wall (where the
  cubes were, now empty). Both halves of the original phrasing
  thus point at the same physical layout via the cube's roll
  axis rather than the "left wall / right wall" labels.

- 2026-05-13 — **Dynamic gateway area.** Second area type built
  around the flipping cube. New `main/areas/dynamic_gateway.{h,c}`
  spawns 3..5 gateway walls back-to-back with two distinguishing
  rules: the hole's x is fixed for the whole area run (chosen once
  from the stage PRNG at init) so the player can pre-align after
  the first wall, and a flipping cube sits right behind each hole
  whose width exactly equals the hole — blocking the passage
  until the cube rolls away as the player approaches.

  Roll direction is always **toward the playfield centre**: hole
  right of centre → cube rolls LEFT, hole left of centre → cube
  rolls RIGHT. The hole-at-exactly-zero edge case snaps to
  left-roll via `dir = (hole_x >= 0.0f) ? -1 : +1`, so there's no
  deadband.

  Geometry: wall slab at `z = WORLD_Z_FAR_SPAWN`, flipping cube at
  `z = WORLD_Z_FAR_SPAWN + (CUBE_GATE_HALF_D + FLIPPING_CUBE_HALF_D)`
  (= +1.2 u behind the wall plane). Hole half-width =
  `FLIPPING_CUBE_HALF_W` (= 0.8). The cube's rolled-AABB outer edge
  ends flush with the inner edge of the wall slab on the roll-
  toward side — the rolled cube exactly fills the gap between
  the wall slab on one side and the hole edge on the other, so
  the safe passage through the hole stays open after the roll.
  Painter's sort renders the cube first (larger z) then the wall
  slabs on top, giving correct occlusion (cube partially hidden
  behind the slab it rolled toward).

  Inter-wall pad = **10 u** (vs. standard gateways's 50 u).
  Standard gateways needs 50 because the hole shifts wall-to-wall
  and the player has to traverse the full track width to re-align.
  Here the hole is fixed for the whole area run so the player
  holds position — the pad only needs to give the cubes room to
  cascade through their roll animations without visually
  overlapping, and 10 u (= 0.5 s at cruise) gives a punchy
  staggered rhythm. Centre-to-centre wall spacing is 10.8 u
  (pad + slab thickness 0.8).

  Booster placement: scoped to the FIRST wall only. The area's
  init clears `passage_mirror` to 0 (overloaded as a "first-wall
  pending" flag for this area type — same int field that
  dynamic_passage uses for its mirror/non-mirror subtype). The
  tick checks that flag on each wall spawn; on the first wall,
  if `boosters_owed > 0` it spawns a booster at
  `(gate_hole_x, WORLD_Z_FAR_SPAWN)` (in the hole, in front of
  the flipping cube) and decrements `boosters_owed`, then sets
  `passage_mirror = 1`. Subsequent walls inside the same area
  don't consume from `boosters_owed`, so any booster the
  scheduler flags mid-area (after the first wall has already
  spawned) carries over to the next area instead. Predictable
  reward: the first wall is the only "free" pass.

  Stage gate: `min_stage = 3`, `max_stage = 6` (overlaps the
  back half of dynamic_passage's 2..5 window, with shared
  real estate at stages 3..5 and a one-stage tail at 6 after
  dynamic_passage has aged out). Wired into the picker
  candidates list and init/tick dispatch in `world.c`; sources
  added to
  `CMakeLists.txt`; new field `float gate_hole_x` on
  `area_state_t` to carry the per-area-run hole x (unused for
  other area types). TAB debug-force target moved from
  `AREA_TYPE_DYNAMIC_PASSAGE` to `AREA_TYPE_DYNAMIC_GATEWAY` so
  the new area is the default smoke-test path.

- 2026-05-13 — **Audio subsystem design locked in.** Phase 7 work
  starts. Top-level architecture is a software mixer with a
  swappable music-source interface so the procedural synthwave
  generator we ship first can be replaced (or A/B'd) with a
  modplayer, MP3 player, or MIDI source later without touching
  the mixer or game code.

  **Pipeline format (locked):** 22050 Hz, signed-16-bit, stereo
  L/R interleaved, throughout. Mixer chunk = 256 stereo frames
  (~11.6 ms). One sample rate for *everything* (music + every
  SFX + I2S output) so there's no resampling on the audio path.
  22050 was picked over 44100 because the procedural synth has
  several voices doing per-sample work and the CPU budget is
  shared with the existing 28-FPS render pipeline; 22050 halves
  the audio CPU bill and is plenty for game audio. The launcher
  mixer (which we adapted) runs 44100; we deliberately diverge.

  **Mixer (`main/audio_mixer.{c,h}`).** Adapted from
  `tanmatsu-launcher/main/audio_mixer.c`. Owns the I2S channel
  exclusively. Single mixer task pinned to core 1 at
  `configMAX_PRIORITIES - 1` (real-time audio priority). Slots
  are typed:
    - **One music slot** holding a `music_source_t*` (or NULL).
      Mixer renders music into a scratch buffer and scales by
      MUSIC_GAIN (≈ 30% of full-scale) before summing — gives
      SFX clear headroom. NULL = silent music slot.
    - **N SFX voice slots** (initial N=8) holding `sfx_voice_t*`.
      Each voice's `render(self, out, frames)` writes into a
      per-voice scratch buffer; mixer sums at unity gain; voice
      sets `self->finished = true` when done and the mixer
      reaps it. One-shots and persistent voices share the same
      mechanism — persistent voices simply never set `finished`
      until their owner stops them.
    - Master gate from `audio_settings`: if `music_on==0` the
      mixer skips the music render call entirely (saves CPU);
      `sfx_on==0` skips the SFX pass.
  Power management mirrors the launcher mixer: after every slot
  has been silent long enough to drain the I2S DMA queue
  (`MIXER_DRAIN_CHUNKS` ≈ 46 ms of silence pushed), the task
  mutes the amplifier and disables the I2S channel, then blocks
  on `ulTaskNotifyTake` until a producer wakes it. Two
  *explicit* shutdown paths exist for the cases the user
  called out (game-over and Esc-to-launcher): clearing the
  music slot + stopping all SFX lets the idle-drain path fire
  within ~50 ms (no "few seconds of noise"); for app exit we
  also call `audio_mixer_shutdown()` which mutes + disables
  synchronously before `bsp_device_restart_to_launcher()`.

  **Music source interface (`main/audio_source.h`).** Trait-style
  function table so the mixer is backend-agnostic:
  ```c
  typedef struct music_source_s {
      void (*render)(struct music_source_s* self,
                     int16_t* stereo_out, size_t frames);
      void (*on_seed)(struct music_source_s* self, uint32_t seed);
      void (*shutdown)(struct music_source_s* self);
      // backend-specific state follows in the embedding struct
  } music_source_t;
  ```
  Each backend (procedural now; future modplayer / MP3 / MIDI)
  exposes a constructor returning `music_source_t*`. Caller
  installs via `audio_mixer_set_music(source)`; mixer doesn't
  care what's behind the pointer. Files for future backends
  will live in `main/modplayer/`, `main/mp3player/`, etc. — one
  directory per backend, mirroring the existing structure.

  **Procedural synthwave generator (`main/music/music_procedural.{c,h}` +
  helpers).** Initial music source. Composition is rule-based:
    - **Section length** ≈ 16 bars. At the start of each section
      the composer may pick a new chord progression, swap drum
      pattern, drop or re-add a layer (bass/arp/pad/drums),
      vary the lead style.
    - **Chord-progression bank**: ~12 canonical synthwave
      progressions (i–VII–VI–VII, vi–IV–I–V, i–v–VI–IV, etc.).
    - **Layers**: bass (saw + lowpass, pulsing 8ths on root),
      arp (square + envelope, walking chord tones), pad
      (detuned saws + slow filter sweep), drums (synthesised
      kick/snare/hi-hat).
    - **Seed-derived determinism**: a **second PRNG** is split
      off at `world_init` from the same level seed:
      `stage_prng = hash(seed, "world")`,
      `music_prng = hash(seed, "music")`. The two never share
      state, so playing the same daily seed with music on vs.
      music off produces identical obstacles. Same custom seed
      ⇒ same music + same world; different seed ⇒ different
      musical personality.
    - **Tempo** ~110 BPM (synthwave canonical). Section/bar/beat
      counters are sample-accurate so the composer can sync
      drum hits and chord changes precisely.

  **Sound effects (`main/sfx/sfx_*.{c,h}` — one .c/.h per effect).**
  Procedurally generated; no PCM data on disk. Five for the
  current feature set:
    - **`sfx_engine_hum`** — persistent. Pitch follows
      `ship_speed_z` (continuous, not stepped — caller pokes a
      `set_pitch(float)` API each frame; the voice reads it at
      the start of each mixer chunk so it stays sample-accurate
      but doesn't fight zero-cross artifacts). Sawtooth + lowpass,
      maybe two detuned saws for thickness.
    - **`sfx_pickup_ding`** — one-shot. Two short
      sine-tone-with-fast-decay-envelope hits a major-third
      apart (like an electric doorbell). ~250 ms total.
    - **`sfx_crash`** — one-shot. Filtered noise burst with a
      pitched low-sine "thud" layered in, fast attack, ~500 ms
      decay.
    - **`sfx_scrape`** — persistent. Bandpass-filtered noise +
      slow LFO on the filter cutoff to get the metallic
      shimmer. Caller controls start/stop and an
      `intensity` parameter (depth of contact).
    - **`sfx_cube_bump`** — one-shot. Deep sine + filtered
      noise for the "thump", maybe ~80 Hz, with a fast pitch
      drop for the impact feel. ~300 ms.
  Each SFX has its own typed API (`sfx_engine_hum_start()`,
  `sfx_engine_hum_set_pitch()`, `sfx_engine_hum_stop()`;
  `sfx_pickup_ding_play()`; …). Internally each returns or
  registers an `sfx_voice_t` with the mixer.

  **Shared DSP primitives (`main/audio_dsp.{c,h}`).** Both the
  procedural music voices and the SFX generators sit on top of
  the same primitive layer:
    - Sine oscillator via a single 1024-entry int16 lookup
      table; saw / square / triangle as cheap arithmetic on a
      phase accumulator; white-noise generator (xorshift on
      uint32, scaled).
    - ADSR envelope (single-precision, lerped between sample
      blocks for cheap evaluation).
    - Biquad filter (lowpass / highpass / bandpass) using the
      standard RBJ cookbook coefficients.
    - Soft-clip / tanh-shape for the final sum to avoid
      clicks when SFX pile up.
  These are *not* shared with the existing render pipeline —
  the audio task owns its own scratch state. Sample lookup
  tables are PSRAM-allocated once at boot.

  **Settings (`main/audio_settings.{c,h}`).** Two u8 NVS keys
  in the existing `synthracer` namespace (no new namespace):
    - `audio_music_on` — default 1.
    - `audio_sfx_on` — default 1.
  Loaded at boot, checkable via `audio_settings_music_on()` /
  `_sfx_on()` (live, no per-frame NVS read). Toggled via a new
  "Audio" menu entry on the main menu — checkbox panel, F4 / B
  to leave. Changes apply live and persist immediately.

  **State-machine integration (`main/main.c`).**
    - Boot: `audio_settings_load()` → `audio_mixer_init(22050)`.
    - On `start_run()`: build `music_procedural_create(seed)`,
      install via `audio_mixer_set_music()`. Start
      `sfx_engine_hum`. Pause-screen keeps both running (music
      *and* hum) — pause is "still in the run".
    - On `STATE_GAME_OVER` entry: `audio_mixer_set_music(NULL)`,
      stop engine hum and any persistent scrape. Idle drain
      mutes the amp within ~50 ms.
    - On F1-to-launcher (or any explicit app exit):
      `audio_mixer_shutdown()` *before*
      `bsp_device_restart_to_launcher()` to silence the
      speaker synchronously.
    - SFX trigger sites: booster pickup callback →
      `sfx_pickup_ding_play()`; collision → `sfx_crash_play()`
      then game over; wall-graze / cube-side-touch →
      `sfx_scrape_start()` while contact persists, `_stop()`
      when contact ends; flipping cube `landing` event →
      `sfx_cube_bump_play()`.

  **What this does *not* commit to yet.** Volume-key handling
  and audio-jack hot-swap (originally planned in this section
  as part of the audio module) still match the launcher
  pattern — that's an integration job that lands separately,
  not a design choice that needed to be made today. System
  volume continues to live in the `"system"` NVS namespace
  (launcher-shared), independent of the new `audio_music_on` /
  `audio_sfx_on` toggles.

- 2026-05-13 — **Master volume + brightness honour the launcher.**
  Cherry-picked the upstream template commit that exports the
  `nicolaielectronics/tanmatsu-settings` module (was
  `tanmatsu-template-grace` `1fa3ab4`; landed in our tree as the
  most recent commit). The new `nvs_settings_*` API gives apps
  read/write access to the same launcher-persisted NVS namespace
  for display backlight, keyboard backlight, LED brightness, and
  speaker/headphone volume.

  New module `main/hw_settings.{c,h}` implements the
  "Respect the master volume" model from
  `../volume_howto.md`:
    - At boot (`hw_settings_init`, called from `app_main` right
      after `audio_mixer_init`): pull each of the five persisted
      values via the upstream helpers, push to its BSP setter
      (`bsp_display_set_backlight_brightness`,
      `bsp_input_set_backlight_brightness`,
      `bsp_led_set_brightness`, `bsp_audio_set_volume`,
      `bsp_audio_set_amplifier`). Audio output (speaker vs.
      headphone) is picked from the initial jack state read via
      `bsp_input_read_action(BSP_INPUT_ACTION_TYPE_AUDIO_JACK)`.
    - On audio-jack hot-swap (event delivered by the BSP as
      `INPUT_EVENT_TYPE_ACTION` /
      `BSP_INPUT_ACTION_TYPE_AUDIO_JACK`):
      `hw_settings_on_jack_event(state)` re-picks the NVS key,
      pushes the persisted value into the codec, and mutes the
      speaker amplifier when headphones are inserted (the codec
      drives both lines but the speaker amp is a separate
      chip).
    - On VOLUME_UP / VOLUME_DOWN navigation keys:
      `hw_settings_step_volume(±5)` reads the active output's
      NVS value, clamps to [0,100], writes it back via the
      upstream `nvs_settings_set_*_volume` helper (so the
      launcher sees the change next boot), and re-applies via
      `bsp_audio_set_volume`. Same 5% step the launcher uses.
  `input.c` dispatches both events directly inside its drain
  loop — no edge latch + main-loop poll, because volume / amp
  routing is system housekeeping rather than gameplay input.
  This intentionally diverges from the rest of `input.c`
  (which buffers everything for `main.c` to consume per frame).

  **Not** mixed up with `audio_settings.{c,h}`:
  the per-app `audio_music_on` / `audio_sfx_on` toggles
  continue to live in our private `synthracer` NVS namespace.
  Those flags gate the procedural synth and SFX render passes
  in the mixer; they're orthogonal to the codec master volume.
  We could refactor `audio_settings.c` to use the upstream
  `nvs_settings_get_u8` / `set_u8` generic helpers — but those
  write to the launcher's namespace and would pollute it with
  app-specific keys, so we keep our own.

  **What's still TODO.** F2 / F3 weren't wired to live
  brightness adjustment yet (the per-frame "dim / bright"
  keys the template originally documented). Boot-time loading
  of the persisted brightness covers the "honour the launcher
  setting" half; the in-app step would be a small follow-up
  using `nvs_settings_set_display_brightness` etc. — same
  pattern as `hw_settings_step_volume`.

- 2026-05-13 — **Audio first-light + tuning pass.** Phase 7
  audio went from designed to making sound during this session.
  Several small bugs and several rounds of subjective tuning;
  capturing each so the reasoning survives.

  **(1) I2S clock reconfigure on an enabled channel.** First boot
  failed with `i2s_std: i2s_channel_reconfig_std_clock(290):
  invalid state` from `bsp_audio_set_rate(22050)`, error 259
  (ESP_ERR_INVALID_STATE). Root cause: the BSP enables the I2S
  channel inside `bsp_device_initialize()` at whatever rate it
  defaults to, and the ESP-IDF i2s_std driver refuses to
  reconfigure the clock while the channel is enabled. The
  launcher's mixer doesn't hit this because the launcher has its
  own `bsp_audio_initialize()` entry point that does
  enable→configure→disable sequencing for us — that entry isn't
  exported through graceloader. Fix in `audio_mixer_init`:
  `bsp_audio_get_i2s_handle()` → `i2s_channel_disable()` →
  `bsp_audio_set_rate(22050)` → `i2s_channel_enable()`, with
  fall-through logging so the failure mode is obvious next time.

  **(2) Crash-time stall: reordered head_on path + deferred save
  commit.** Original flow called `save_commit_run_end()` (NBT
  serialise + `fastopen("wb")` + write + close — synchronous
  flash I/O, measured at hundreds of ms) *first* inside the
  PLAYING head_on branch, then queued the crash SFX, then
  transitioned to GAME_OVER. Result: the main loop blocked on
  the flash write before the crash sound was even registered,
  and the audio task on core 1 hit a brief cache-disable
  cascade during the write that made the I2S DMA loop its
  previous buffer (player heard the music's last second
  repeating, then a delayed crash). New order in
  `main.c:APP_STATE_PLAYING`:
    1. `sfx_crash_play()` — audio task gets the new voice
       *before* the stall begins, so it has fresh PCM to feed
       I2S even if its code section hiccups briefly.
    2. `end_run_audio()` — drop music + persistent voices.
    3. Transition `app_state` and input mode.
    4. Save commit is deferred to the first GAME_OVER frame
       (guarded by the existing `run_end_committed` flag) — the
       slow flash write now hides behind a static screen
       instead of mid-action. `commit_run_end()` logs its
       wallclock so we can watch it shrink/grow as the save
       format evolves.

  **(3) Master gain staging in magicnumbers.h.** Initial design
  put music at a hard-coded 30% gain and SFX at unity, with each
  SFX file owning its own amplitude constant. After tuning by
  ear ("the crash is too loud" → "now they're all too quiet" →
  "this still clips when scrape + ding + cube_bump fire near
  each other") it became clear the per-effect amps were doing
  two unrelated jobs: relative loudness *between* effects and
  the overall SFX-bus level *against* music + the int16 clip
  ceiling. Split them:
    - `AUDIO_MUSIC_GAIN  0.30f` and `AUDIO_SFX_GAIN 0.35f` in
      `magicnumbers.h` set the bus levels.
    - The mixer applies both as Q15 multipliers at sum-in time
      (`audio_mixer.c`).
    - Per-effect `*_AMP` constants now tune relative loudness
      only, in the 0.4–0.9 range.
  Headroom budget: 5 random-phase concurrent SFX + music + hum
  must stay under the ±1.0 hard-clip ceiling. With per-voice
  nominal amps ~0.5 and SFX gain 0.35, effective per-voice peak
  is ~0.18; √5 random-phase summing puts 5 concurrent voices
  around 0.40, leaves room for music (0.30) + hum and stays
  comfortably under 1.0.

  **(4) Hum, volume, and balance.** Several rounds:
    - `HUM_AMP` 0.10 → 0.04 first (felt too prominent in the
      bed), then 0.04 → 0.11 *nominal* when the master SFX gain
      landed (× 0.35 master ≈ 0.04 effective — same perceived
      level, just re-expressed in the new gain-staged units).
    - One-shot amps rebalanced into the new gain stage: ding
      0.50, crash 0.45 + 0.40, scrape 0.40 max.
    - Perceived loudness is logarithmic — every numeric drop
      here is much smaller perceptually than instinct
      suggests. "Twice as loud" is +10 dB ≈ ×3.16, not ×2.

  **(5) Engine-hum toggle (third audio settings checkbox).** Some
  players find the constant low drone fatiguing even when they
  want one-shot SFX on. Added `audio_hum_on` u8 to the
  `synthracer` NVS namespace (default 1), exposed as a third
  entry in the Audio settings panel (`AUDIO_ENTRY_HUM`).
  Two-layer gate: `start_run()` skips
  `sfx_engine_hum_start()` when the flag is off (the efficient
  path — voice never registers with the mixer), and the hum
  voice's `render()` callback also defensively returns silence
  when the flag is off (the live-toggle path in case we ever
  expose the menu from PAUSED, where the voice is already
  registered).

  **(6) Cube-bump shape: kick-drum, not noisy clonk, *and*
  speaker-audible.** Two passes.

  *First pass* — replaced the original noise-heavy click with a
  pitched sine sweep (190 → 55 Hz) + sub-octave + brief lowpassed
  click transient. Critical bug spotted on listening: the
  sweep was tied to the body envelope's total length, so making
  the decay longer (for "ringier" tail) also slowed the pitch
  drop into a glide and the sound stopped reading as a hit.
  Fix: decoupled the sweep clock from the envelope —
  `BUMP_SWEEP_S` is independent of `BUMP_BODY_DECAY_S`. Pitch
  settles within ~90 ms, body envelope rings out for ~700 ms at
  the settled low frequency. That long sustained low tail is
  what makes a kick sound "big".

  *Second pass* — headphone listeners reported it sounding
  weighty, but on the built-in speaker the wump was almost
  inaudible (came across as a click). Built-in speaker rolls
  off hard below ~200 Hz, so the 55 Hz fundamental physically
  doesn't come through. Solution: add the **harmonic series**
  (2× and 3× the fundamental, phase-locked via integer
  multiples of `inc_f`) at meaningful amplitudes — the speaker
  reproduces those (110 / 165 → 380 / 570 Hz over the sweep)
  and the brain reconstructs the missing fundamental via the
  missing-fundamental psychoacoustic effect. Same trick phone
  and laptop speakers use to fake their entire bass response.
  Headphone listeners still get the fundamental + sub-octave
  for chest-thump weight; speaker listeners get the apparent
  bass via the harmonic stack without any energy wasted on
  inaudible low end. Final per-voice layers: fundamental
  (0.70), 2nd harmonic (0.40), 3rd harmonic (0.22), sub-octave
  (0.25, headphone-only), click transient (0.22).

  **(7) Pause stops SFX, not music.** Hitting F4 mid-game now
  calls `pause_audio_for_pause_menu()`: stops the engine hum
  voice, stops the scrape voice, and marks all in-flight
  one-shot voices finished via `audio_mixer_stop_all_voices()`.
  Music keeps playing through the pause overlay (matches the
  long-standing design decision that pause is "still in the
  run"). Resume calls `resume_audio_from_pause_menu()` which
  restarts the hum if the player has it enabled; scrape and
  one-shots re-arm naturally via their per-frame collision /
  event triggers. The `s_scrape_was_on` edge tracker moved
  from a function-local static inside `APP_STATE_PLAYING` up
  to file scope so the pause helper can reset it — otherwise a
  scrape held over the pause boundary would skip its restart
  edge and stay silent the rest of the contact.

- 2026-05-13 — **Phase 6 design update: Tri pickups + pickup-event
  scoring + top-left multiplier HUD.** The original Phase 6
  sketch (`Implementation Phases` numbered item 6, plus the
  `game.c` Module Responsibilities Multiplier bullet) was a few
  short lines. Updating it now in the same pre-implementation
  pass that finished Phase 7, while the design choices are fresh.

  **What's diverging from the original plan.**
    - **Scoring adds pickup-event bumps on top of the continuous
      distance income** (refined 2026-05-14 after an initial
      pickup-only proposal was over-aggressive — the original
      Race The Sun model is distance-primary, and removing it
      left the score stuck between bonus events). Final model:
      `score += dz × multiplier × DISTANCE_FACTOR` per frame
      (the always-on income stream, mirrors RTS), plus
      **Tri pickup = +5 × multiplier** and **booster pickup
      = +10 × multiplier** as discrete bonus bumps the player
      perceives as "the number jumped".
      `GAME_SCORE_DISTANCE_FACTOR` is 0.1, so the per-frame
      baseline is one-tenth of what it would be at unit gain —
      enough to keep the score climbing visibly between
      pickups, but not so much that a Tri's +5 disappears as
      noise on top of it. All three streams are multiplied by
      the same `multiplier` so a Tri-bumped multiplier
      amplifies forward distance income too.
      `distance_traveled` stays separately tracked for the
      stats panel.
    - **Multiplier mechanic: fill-and-reset, not accumulating.**
      Original sketch said "every 5 Tris collected → +1". New
      design: a visible Tri counter goes 0/5 → 1/5 → 2/5 → 3/5 →
      4/5 → multiplier++, counter resets to 0/5. The fill-and-
      reset model gives a clear visible "almost there" / "just
      bumped" rhythm in the HUD instead of a counter that only
      moves arithmetically. Crash penalty (-5 to multiplier,
      floor at 1 today) is unchanged from the original sketch;
      Phase 11 will adjust the floor with metaprogression
      level.

  **Tri object — copy, don't share.** New
  `main/objects/tri.{c,h}`, **copied** from `main/objects/
  booster.c` (the existing pyramid-shaped pickup) rather than
  shared. Same pyramid geometry, recoloured blue (front /
  side / top / outline all in the cyan-blue band that matches
  the synthwave palette). Copy-don't-share so the two objects
  can drift apart independently — tris probably want their
  own size, pulse rate, and spawn cadence over time without
  being constrained by the booster's tuning. Cost is small
  duplication (~150 lines).

  **Scoring constants.** New `magicnumbers.h` block:
    - `GAME_SCORE_TRI         5`
    - `GAME_SCORE_BOOSTER    10`
  Both are int (score is an integer count, not a float —
  fits the user's "basis points" phrasing). `game.score`'s
  current `double` type stays the same so we don't have to
  touch save-format code; we just store integer values in it.

  **Multiplier HUD — top-left grey box (refined 2026-05-14:
  four slots, not five).** A new opaque (not translucent)
  dark-grey panel anchored at the top-left corner, containing
  two visual rows:
    1. **Tri progress** — **four** small triangles, evenly
       spaced left-to-right, filled in *blue* for collected
       and *black* (outline only, or solid dark grey) for
       the remaining slots toward the next multiplier bump.
       Why four and not five: the 5th Tri *immediately* ticks
       the multiplier and the counter resets to 0, so the 5th
       slot would never be visually held — the moment it
       would light up, the whole row resets. With four slots
       the display goes 0/4 → 1/4 → 2/4 → 3/4 → 4/4 (all lit)
       → next Tri pops the multiplier and clears back to 0/4.
       Lit-slot count = `pickups_tri % 5` (which is always in
       {0,1,2,3,4}, so it maps directly without a clamp).
    2. **Current multiplier** — single line of text rendered
       below the triangle row, e.g. `×3`.
  The panel is *opaque* (unlike the menu dim-rect helper) so
  the multiplier readout is always legible regardless of what
  scenery sits underneath it. Suggested geometry: ~120 × 60
  px panel, 4 triangle icons ~22 px wide at 4 px spacing,
  multiplier text at ~28 px line height below.

  **Plink note count is unchanged.** Even though the HUD only
  shows four slots, the ascending plink SFX still plays five
  distinct notes per multiplier cycle: pickups 1–4 audibly
  fill the HUD (C5/D5/E5/G5), and pickup 5 — the one the HUD
  never displays — plays the highest note (A5) at the same
  moment the multiplier ticks. The 5th plink *is* the
  "multiplier bumped" feedback; the HUD reset is the visual
  confirmation. Slot index for the plink stays
  `(pickups_tri - 1) % 5` so all five pitches still get used.

  **HUD shuffle: push F1 / F4 hints down.** The existing
  `draw_exit_hint()` (F1 icon at `y = 12`) and
  `draw_pause_hint()` (F4 icon at `y = 34`) currently occupy
  the corner where the new multiplier panel goes. Move both
  hints down to clear the panel — easiest is to parameterise
  their `y_top` (or just hardcode a new baseline of
  `panel_bottom + 8`). The pause hint stays one line below
  the exit hint, same +22 px spacing as today.

  **What's already in place (no work needed).**
    - `game_state_t.pickups_tri`, `.score`, `.multiplier`,
      `.multiplier_max` fields already exist.
    - `run_stats_t` already persists `pickups_tri` (sum-merged
      into all-time on every `save_commit_run_end`) and
      `multiplier_max` (peak-tracked).
    - `OBSTACLE_KIND_PICKUP_TRI` enum value already exists,
      with a `hit = OBSTACLE_HIT_IGNORE` placeholder in the
      collision dispatcher ready to be replaced by the real
      collect path.

  **Tracking semantics — `pickups_tri` is monotonic per run.**
  The HUD's 0/5 fill-and-reset display is purely cosmetic —
  it reads `pickups_tri % 5` rather than carrying its own
  state. The underlying `game_state_t.pickups_tri` counter
  ticks up by 1 on every collect for the entire run and is
  never reset mid-run (not by crashes, not by multiplier
  bumps). At `save_commit_run_end` it gets copied into
  `last_run.pickups_tri` and summed into
  `all_time.pickups_tri` by the existing run_stats merge.
  The all-time total is therefore the player's lifetime Tri
  count across every saved run on the active slot, and the
  per-run total is the count for that specific run (also
  visible on the stats screen via `last_run.pickups_tri`).
  Storing the total rather than the modulo means the stats
  panel doesn't need any new bookkeeping.

  **Multiplier-bump condition.** With the monotonic counter,
  the multiplier ticks on the precise transitions: bump when
  `pickups_tri % 5 == 0 && pickups_tri > 0` after the
  increment. Crash penalty stays on the multiplier itself
  (-5, floor 1); the Tri counter and HUD progress are not
  rewound by a crash — that would feel doubly punishing for
  a single mistake.

  **Pickup SFX: melodic ascending plink.** New
  `main/sfx/sfx_pickup_plink.{c,h}` — distinct from the
  booster's `sfx_pickup_ding`. Frequency steps with the
  Tri's slot in the current cycle so the player audibly
  hears the HUD filling up. Use a C-major-pentatonic
  ascending pattern (C5, D5, E5, G5, A5 — frequencies
  523.25 / 587.33 / 659.25 / 783.99 / 880.00 Hz) so the
  five pickups across a multiplier cycle form a brief
  musical phrase that resolves on the 5th note as the
  multiplier bumps. Implementation: pass the slot index
  (0..4 = `(pickups_tri - 1) % 5` after increment) to the
  factory function — e.g. `sfx_pickup_plink_play(int
  slot_index)` — and the voice picks the frequency from a
  static lookup. Shape: short sine pulse, ~5 ms attack,
  ~140 ms decay, no noise component (it should sound
  bell-like, not a percussion hit). Amplitude similar to
  the ding (around 0.50 per-voice nominal, scaled by the
  SFX master gain).

  **Estimated touch points.**
    - `main/objects/tri.{c,h}` — new, copied from
      `objects/booster.c`.
    - `main/sfx/sfx_pickup_plink.{c,h}` — new, ascending-
      pentatonic pickup tone driven by the in-cycle slot
      index.
    - `main/magicnumbers.h` — `GAME_SCORE_TRI`,
      `GAME_SCORE_BOOSTER`, optional tri-specific tuning
      constants.
    - `main/game.c` — replace the `IGNORE` placeholder in the
      `OBSTACLE_KIND_PICKUP_TRI` case with the real collect:
      increment `pickups_tri`, bump score by `GAME_SCORE_TRI
      × multiplier`, fire `sfx_pickup_plink_play((pickups_tri
      - 1) % 5)`, and if `pickups_tri % 5 == 0` then
      `multiplier++` (the HUD reads `pickups_tri % 5` so no
      explicit counter reset is needed). Add the
      `+= GAME_SCORE_BOOSTER × multiplier` to the existing
      booster pickup branch. The per-frame
      `score += dz × multiplier` distance accumulator in
      `game_after_collide` stays put — pickups stack on top
      of it.
    - `main/world.c` or one of the area generators — add Tri
      spawn cadence (probably scattered through
      `pixel_field` / `big_blocks` at ~5–8 per stage to start
      with, tuneable).
    - `main/main.c` — new `draw_multiplier_panel()` helper
      (opaque grey rect + **4** triangles + multiplier text);
      `draw_exit_hint()` / `draw_pause_hint()` baseline
      pushed down. No separate "multiplier bumped" cue —
      the 5th plink resolving on the high pentatonic note,
      played at the same instant as the HUD resets to 0/4,
      is the audio + visual confirmation together.
    - `main/CMakeLists.txt` — `main/objects/tri.c` and
      `main/sfx/sfx_pickup_plink.c` added to `APP_SOURCES`.

  **Per-area Tri spawn rules (locked 2026-05-14).** Each area
  type knows how to populate itself with obstacles; the Tri
  spawner is the same shape — owned by the area, not by a
  global rule — so the cadence and density can be tuned per
  area without one rule's edge cases pulling on every other
  area. Locked rules:

  - **`pixel_field`** — Tri count = **half** the number of
    pixel-cube blocks the area would spawn. Tris are placed
    at random positions within the playfield, with collision
    avoidance: must not embed in either border wall, must not
    overlap any other already-placed obstacle (cube or other
    Tri). Pixel field emits blocks on a cadence
    (`PIXEL_INTERVAL_Z`), so simplest implementation is a
    second cadence at twice the interval that emits a Tri,
    each rejection-sampled against the obstacle pool. Cap
    retries (e.g. 8) and skip the slot rather than blocking
    on a full pool.

  - **`big_blocks`** — Tri count = **equal to** the number of
    big-cube blocks. Same placement + collision-avoidance
    rule as pixel field. Tighter packing because there are
    fewer / larger blocks; rejection sampling still works.

  - **`gateways`** — every gateway hole that's **not** holding
    a speed booster gets a Tri in the hole instead. The
    Tri sits at `(hole_x, WORLD_Z_FAR_SPAWN, 0)` — same z as
    the wall slabs, centred on the chosen hole. The
    boosters-owed gate already decides "booster vs no
    booster" per gateway; we simply add a `// else spawn
    Tri` branch.

  - **`dynamic_gateway`** — same rule as `gateways`: every
    wall not consuming a booster from `boosters_owed` gets a
    Tri in its hole. The flipping cube behind the hole is
    a separate object — the Tri sits in front of (or
    co-located with) the booster slot at
    `WORLD_Z_FAR_SPAWN`, not at the cube's z offset.
    Because the hole position is fixed per-area in this
    type, every Tri in a given dynamic_gateway area lands
    at the same x.

  - **`dynamic_passage`** — one Tri **per flipping cube**.
    Placement is in the heavy pixel-field clutter that runs
    alongside the flipping cubes, randomly within the
    playfield with the same collision-avoidance rule as the
    standalone pixel_field. The Tri's z is whatever the
    clutter scheduler emits at (i.e. it goes into the same
    emission timer as the clutter pixel cubes, displacing
    the next cube slot rather than overlapping it). Yields
    4–7 Tris in a dynamic_passage area (one per flipping
    cube, count is `PASSAGE_FLIP_MIN`…`PASSAGE_FLIP_MAX`).

  - **`bridges`** — one Tri **per bridge**. Placement is a
    **straight line** across all the bridges' z positions,
    with the line's start-x and end-x picked once per area
    from `[-TRACK_HALF_WIDTH, +TRACK_HALF_WIDTH]` (with a
    small inset for the Tri's half-width so the endpoints
    don't clip the border walls). The line can be at any
    angle including degenerate (start ≈ end). Each Tri's z
    is the corresponding bridge's z, x is the lerp position
    along the line. Single line per area drawn at init;
    bridge-tick co-emits the Tri when it emits each bridge.
    No collision avoidance needed against the bridge
    itself — bridges are visual-only spans the player flies
    under, the Tri at ground level under the bridge is
    geometrically in a different y-band.

  - **`rest` (except the pre-stage-1 lead-in)** — 10 Tris in
    a **smooth S-curve**, plus the speed booster as the 11th
    element on the same curve. Curve construction:
      - **Start x** is procedurally picked at init: midway
        between the centre line and one of the two walls,
        i.e. `start_x = ±0.5 * TRACK_HALF_WIDTH` with the
        sign drawn from `world_xorshift32`. So the first
        Tri sits at half-width-left or half-width-right of
        centre.
      - **End x** is the mirror of the start (sign flipped),
        so the curve sweeps from one half-width offset to
        the opposite one.
      - **Tangent at start** is along +z (straight ahead),
        and the curve bends toward the end x over its
        length. A quadratic Bézier with control points
        `P0 = (start_x, z0)`, `P1 = (start_x, z_mid)`,
        `P2 = (end_x, z_end)` does this exactly: zero
        derivative in x at t=0 (the "starts out straight"
        property), smooth bend to the mirror by t=1.
        Sample at `t = i / 10` for `i = 0..10` to get the
        11 points: positions 0–9 are Tris, position 10 is
        the booster.
      - The booster placement here **overrides** the usual
        booster scheduling: rest areas already exempt
        themselves from the stage's booster cadence, so
        this just means "always spawn one booster, at the
        curve endpoint." `boosters_owed` is still consumed
        (= 1) so we don't double-spawn one later.
      - The **pre-stage-1 rest** (the lead-in before
        stage 1 starts) is deliberately excluded — the
        player hasn't seen a Tri yet, the HUD multiplier
        panel reads 0/5 with multiplier ×1, and we want
        them to encounter Tris in gameplay first rather
        than discover them in a rest area before learning
        the rules. The world generator already marks this
        rest specially (banner reads "STAGE 1" once it
        leaves, while regular rest areas show "STAGE N+1").
        Reuse that distinguishing condition.

  **Implementation note on rejection sampling.** The
  pixel_field / big_blocks / dynamic_passage rules use
  random placement with collision avoidance against the
  existing obstacle pool. The pool is small (≤ 512 entries,
  most active at any time ≈ tens), so an O(N) lateral-
  distance check per retry is cheap. Standard pattern:
  draw a candidate `(x, z)`, walk the active pool, reject
  if any obstacle's footprint overlaps (with a small
  padding for visual breathing room — maybe 1.5×
  `TRI_HALF_W`). Cap retries at 8; if the slot can't be
  filled, skip silently. Tris are scoring incentive, not
  mandatory path elements — one missed slot per area is
  fine.

  **Booster shape upgrade — pyramid → rotating regular
  icosahedron (added 2026-05-14).** Since the Tri inherits
  the pyramid shape (copy-don't-share), the speed booster
  needs a fresh visual identity so the two pickups read as
  obviously different at a glance. Move the booster to a
  **slowly-rotating regular icosahedron** — looks like a
  faceted gem, fits the synthwave aesthetic, and is
  visually unambiguous next to the Tri's pyramid.

  Geometry. The regular icosahedron has 12 vertices, 20
  triangular faces, 30 edges. Vertices using the golden
  ratio φ = (1+√5)/2 ≈ 1.618:
  ```
      (0, ±1, ±φ), (±1, ±φ, 0), (±φ, 0, ±1)
  ```
  All 12 vertices sit at distance `√(1+φ²) = √(2+φ) ≈ 1.902`
  from the origin. Scaled so the resulting body fits within
  `GAME_BOOSTER_HALF_W` (0.4u) — i.e. multiplied by
  `GAME_BOOSTER_HALF_W / √(2+φ)`. The icosahedron's existing
  AABB (collision footprint) is unchanged; the visual is
  the only change. Face and edge tables are `static const`
  arrays in `objects/booster.c`.

  Rotation. Continuous rotation around the Y axis (vertical)
  at ~1 full rotation per second. Rotation phase is a
  per-instance float in the obstacle's 128-byte scratch
  buffer; the booster's physics callback advances it by
  `2π / period × dt` each frame. The Y-axis is the natural
  choice — the icosahedron has a 5-fold rotation symmetry
  axis through opposite vertices, and putting one of those
  vertices at the top gives it a clean spinning-gem look.
  All boosters in the scene share phase so they tick in
  lockstep (same trick the current pulse uses — one global
  `time_s` value rather than per-obstacle state).

  Rendering. Existing `render_booster_pyramid()` is replaced
  by `render_booster_icosahedron()` along the same code
  path (the kind-dispatched render in `render.c`'s pickup
  branch). Per-frame steps:
    1. Apply rotation matrix (just `cos θ`, `sin θ` around
       Y) to the 12 vertex positions. ~24 multiplies per
       booster — trivial.
    2. Translate to world position, project the 12 vertices
       via `render_project()` to 2D screen-space.
    3. For each of the 20 faces, compute the face normal in
       world space (rotated), back-face cull if `n · (cam -
       face_centre) ≤ 0`. ~10 faces survive on average.
    4. Draw each visible face via `direct_565_tri` with
       a face-tinted colour — keep the green booster
       palette but vary brightness by face normal vs an
       imaginary light direction (one dot product per
       face) so the gem looks faceted rather than flat.
       Cheap "lighting": tint factor =
       `0.6 + 0.4 * max(0, dot(face_normal, light_dir))`,
       with `light_dir` set to a fixed top-down-ish
       vector.
    5. Stroke every visible edge with
       `GAME_BOOSTER_OUTLINE_COLOR` (the existing bright
       green) via `direct_565_line`. The wireframe + the
       face fills together is what makes it read as a
       glowing gem rather than a faceted blob. Edge list
       is built from the face list at compile time (each
       edge is shared by exactly two faces, draw each
       once even if both faces are visible).

  Pulse behaviour. The existing booster has a brightness
  pulse driven by `GAME_BOOSTER_PULSE_PERIOD_S /
  GAME_BOOSTER_PULSE_AMPLITUDE`. The rotation gives the
  icosahedron its own attention-grabbing motion, so the
  pulse is redundant — **drop the pulse**. The pulse
  constants in `magicnumbers.h` can stay parked
  (`PULSE_AMPLITUDE` = 0 effectively disables it) or be
  removed if no other object grows to use them.

  Tunables (suggested defaults in `magicnumbers.h`):
    - `GAME_BOOSTER_ROTATION_PERIOD_S 1.0f` — one full
      rotation per second; slow enough that the player
      can read each face, fast enough to draw the eye.
    - The icosahedron's vertex / face / edge tables are
      hardcoded in `objects/booster.c` — they're not
      tunable, they're geometry.

  Touch points (added on top of the Phase 6 list above):
    - `main/objects/booster.c` — new vertex / face / edge
      tables, rotation phase in scratch, new draw callback
      (or kind-dispatched render function in `render.c`).
    - `main/render.c` — replace `render_booster_pyramid()`
      with `render_booster_icosahedron()`. The new function
      lives alongside the old until the migration is done.
    - `main/magicnumbers.h` — add
      `GAME_BOOSTER_ROTATION_PERIOD_S`. Pulse constants
      can be set to 0 amplitude or removed.

  Why icosahedron specifically (and not, say, a dodecahedron
  or a sphere). Icosahedron has the most triangular faces
  of the regular polyhedra (20) — that gives it the most
  facets to catch the light while still being clearly a
  *gem* and not a low-poly sphere. The dodecahedron's
  pentagonal faces would force a face-triangulation step in
  the renderer. A UV-sphere would need many more faces to
  look round and would defeat the "faceted gem" aesthetic.
  The icosahedron is the right primitive for "slowly-
  rotating loot crystal".

  **Collision stays AABB — visual-only change.** The current
  collision model (`game_collide()` in `main/game.c`) is a
  swept 2D AABB on the **xz-plane**: each obstacle has
  `(x_world, z_world, half_w, half_d, height)` stored at
  spawn, but the actual head-on / scrape / pickup test
  walks the pool and tests only **x-overlap + swept-z-overlap**
  against the ship's AABB. There is no y-axis test — the
  ship is treated as always at `y = 0`, no "fly under"
  semantics today. The `height` field is metadata: it
  drives shadow-length rendering and is reserved for a
  future jump pickup that would unlock fly-under behaviour,
  but it's not part of any current collision dispatch.

  Consequence for the icosahedron upgrade: **collision
  math is unchanged**. The booster's AABB at spawn
  (`half_w = GAME_BOOSTER_HALF_W = 0.4`, same for `half_d`,
  `height = GAME_BOOSTER_HEIGHT = 0.8`) stays the same; the
  icosahedron is scaled to sit inside that 0.8 × 0.8 × 0.8
  bounding box. Same player-favouring "AABB is generous"
  feel the existing pyramid pickup has — corner-clip near-
  misses still count as collected, by design. Per-vertex /
  per-face collision against the rotating icosahedron is
  *not* added (would cost ~12 rotated-vertex transforms
  and per-face plane tests every frame per booster, with
  no gameplay benefit since the AABB is already
  intentionally forgiving). A tighter cylindrical
  `(Δx² + Δz²) < r²` check is parked as a possibility
  via the existing per-obstacle `collide` callback hook
  (same hook `flipping_cube` uses today) — easy to add
  later if playtesting shows the AABB is *too* generous,
  but not done in Phase 6.

  **What's deliberately not in Phase 6.**
    - Metaprogression multiplier floor (lv6 → 2 etc.) — Phase 11.
    - Tri colour / shape variation per region — Phase 13.
    - "Air tris" / "tris in one region" challenge variants —
      Phase 11.

- 2026-05-15 — **No clock-rollback anti-cheat; custom-seed runs
  award meta-progression.** Two Phase 8 design changes, both
  simplifications:
    1. The planned NVS "last-known-good date" anti-cheat is
       dropped. This is an offline, single-player, open-source
       game with no online leaderboard — there is nothing to
       protect, and policing the clock only adds failure modes
       (an unset or drifted RTC locking the player out of the
       current daily seed). `meta.last_seen_date` survives, but
       purely as a day-rollover detector: on boot, if today's
       RTC date differs from it, reset the `daily_done_*`
       challenge-completion booleans and store today. Rolling
       the clock back simply replays that day's world.
    2. Custom-seed runs now award meta-progression (challenge
       points, level-ups, unlocks) **exactly like daily runs**.
       The original 2026-05-07 decision withheld it to stop the
       player farming easy seeds, but with no leaderboard the
       only person affected is the player themselves — gating
       their own progression behind "you must use today's seed"
       is friction with no payoff. The `is_custom` flag is
       removed entirely: every run feeds `meta.c` the same way,
       and `world_init()` takes just the seed. `last_custom_seed`
       remains, only to prefill the seed-input screen.

- 2026-05-15 — **Menu redesign: Settings submenu + Controls + key
  remapping.** The main menu's "Audio" entry became "Settings", a
  submenu with two children: "Controls" and "Audio" (the Audio child
  is the unchanged three-checkbox panel). New app states
  `APP_STATE_SETTINGS`, `APP_STATE_CONTROLS`, `APP_STATE_KEY_CAPTURE`.
    - **Controls screen** — five rows: a "Gyroscope" enable checkbox
      (default off; stored but not wired to gameplay — reserved for a
      future gyro-steering implementation) followed by four
      remappable keybinds: Left (ESC), Right (Backspace), Use item
      (Space — stored/displayed only, not wired yet) and Pause (F4).
      Each keybind row shows its current key: a PNG icon for Esc and
      F1..F6 (loaded by `icons.c`), otherwise a text label — a word
      for non-printable keys (Tab, Ctrl, Fn, F7..F12, …) or a single
      glyph for printable keys. If a mapped icon fails to load the
      row falls back to the text label.
    - **Remapping** — selecting a keybind row opens a modal
      "Press a key" dialog; `input.c` enters a capture mode where the
      next plain key press is latched and all other input is
      swallowed. Captured from scancode events, plus F-key navigation
      events translated to scancodes as a safety net (function keys
      can surface on either channel depending on the BSP build).
    - **Persistence** — new `controls_settings.{c,h}` module, modelled
      on `audio_settings`: device-global NVS keys in the `synthracer`
      namespace (`ctl_gyro` u8, `ctl_k_left/right/item/pause` u16).
      Global, not per-save-slot — a player's key layout shouldn't
      reset when they switch slots. Keybinds are stored as raw BSP
      scancodes: a scancode identifies any physical key and works
      with both the polled (`bsp_input_read_scancode`, smooth
      steering) and event-matched (pause edge) input paths. The old
      hard-coded ESC/Backspace steering and the navigation-event F4
      pause were replaced by reads of these binds.
    - **Left-alignment** — every menu and dialog (slot select, main
      menu, settings, controls, audio, seed input, stats, upgrade
      stub, pause overlay, game-over overlay, key-capture modal) was
      converted from centred text to left-aligned.
    - **Selection highlight** — a selected row turns yellow
      (`0xFFFFFF6B` vs white) *and* shows a `>` chevron, but its
      label never moves and never changes size. Every row's label
      starts at a fixed `text_x`; a fixed-width gutter to its left
      holds the chevron, which is painted as a **separate draw step**
      only for the selected row. So the chevron-vs-space width
      difference of the proportional Hershey font can't shift the
      label.
    - **Pause-menu access** — the Settings submenu (Controls + Audio)
      is reachable mid-run via a "Settings" entry on the pause
      overlay, not only from the main menu. A file-static
      `s_settings_origin` (`APP_STATE_MENU` vs `APP_STATE_PAUSED`)
      records where Settings was opened from: Esc walks back there,
      and `draw_settings_scene()` renders the frozen game behind the
      panel (matching the pause overlay) when the origin is the pause
      menu. The run stays logically paused throughout — no audio
      resume until the real Resume. Audio/keybind toggles are safe to
      change mid-pause: the mixer reads the audio flags live every
      chunk and the keybinds are polled live each frame, so nothing
      latches at run start.
    - **Unified `menu_draw()` renderer** — the per-menu hand-coded
      draw loops were replaced by one generic list renderer driven by
      a `menu_view_t` (title, optional subtitle, rows, cursor, hint,
      panel size). A `menu_row_t` row is a plain label, a checkbox,
      or a keybind value. Main menu, Settings, Controls, Audio and
      the pause overlay are now just data + one `menu_draw()` call.
      Slot select (two-line rows) and the dialogs (seed input, stats,
      game over, key capture) keep bespoke draws but share the same
      colour constants, `draw_left()` / `draw_chevron()` helpers and
      the chevron-gutter convention.
    - **Credits screen** — a "Credits" main-menu entry opens
      `APP_STATE_CREDITS`, a manually-scrolled credits roll
      (`credits_lines[]`). It is taller than the panel and scrolled
      with UP / DOWN. The Hershey text renderer writes pixels
      directly and ignores `pax_clip`, so lines outside the viewport
      are culled whole rather than clipped; the scroll offset is
      clamped to `credits_max_scroll()`. Section headings (lines
      ending in ':') render yellow. Adding the 7th main-menu row
      meant tightening that menu's row pitch from 44 → 38 px.
    - **"F1 to exit" hint removed.** The icon+text exit hint that
      `draw_exit_hint()` painted top-left on every screen is gone —
      it advertised a dev-only fast-exit. F1 *still* exits to the
      launcher (the input handler is unchanged); it just isn't shown.
      `draw_exit_hint()` and its 12 call sites were deleted, and the
      "F4 to pause" gameplay hint (`draw_pause_hint()`, a real player
      affordance, kept) moved up to `HUD_HINT_Y_BASE` to close the
      gap the exit hint left. The full F1-exit affordance is itself
      slated for removal later in development.

- 2026-05-15 — **Gateway dead-air trimmed.** Both gateway-type
  areas (`gateways`, `dynamic_gateway`) carried a `*_SETTLE_Z`
  equal to a full far-plane distance (`WORLD_Z_FAR_SPAWN`, 100 u =
  5 s at cruise) prepended to the lead-in pad, to let the previous
  area's last obstacle drain off-screen before the gate puzzle. At
  150 u (7.5 s) before the first `gateways` gate this read as a
  long empty crawl. The settle was removed entirely: the empty
  run before the first gate and after the last is now exactly one
  inter-gate pad (`GATEWAY_PAD_Z` 50 u / `DYN_GATE_PAD_Z` 10 u),
  matching the gate-to-gate spacing. Safe because gates always
  spawn at `WORLD_Z_FAR_SPAWN`, the furthest spawn depth — a fresh
  gate is never placed behind a leftover foreign obstacle; any
  leftover is strictly nearer and drifts past before the gate
  reaches the player. *(Supersedes the 2026-05-11 "Gateway
  settling pad" entry.)*

- 2026-05-15 — **Rest-area: banner timing, short intro, green
  pillars.** Three changes to make rest areas read better:
    - **Stage banner timing.** The "Stage: N" banner was shown for
      the *whole* rest area, so it popped up the instant the rest
      stretch first appeared on the far horizon — far too early.
      It now shows only during the final `STAGE_BANNER_LEAD_Z`
      (100 u, ≈5 s at cruise) of the rest stretch, i.e. as the
      player crosses from the previous zone's bonus tail into the
      next zone. A shared `stage_banner_visible()` helper gates all
      three render paths (playing / paused / game-over).
    - **Short intro lead-in.** The pre-stage-1 rest is now one
      screen depth (`WORLD_Z_FAR_SPAWN`, 100 u) instead of a full
      `WORLD_REST_LENGTH_Z` (200 u) rest, so the run opens briskly.
      Since that equals the banner threshold, the "Stage: 1"
      banner stays visible for the whole intro. `area_rest_init()`
      gained a `length_z` parameter — between-stage rests still
      pass `WORLD_REST_LENGTH_Z` (the 10-Tri Bézier curve is
      unaffected); the intro passes `WORLD_Z_FAR_SPAWN`.
    - **Green border-wall posts.** New `objects/rest_pillar.{c,h}`
      — green marker posts modelled on the bridges-area pillars but
      half height (`REST_PILLAR_HEIGHT` 1.5 u) and with no
      connecting span slab. `area_rest_tick()` (previously a pure
      countdown) now spawns a left+right post pair every 9 u
      (3 wall segments, matching the bridges pillar cadence,
      wall-grid-snapped) for the whole rest stretch, via the
      formerly-unused `next_event_z` field. The posts sit on the
      wall tops outside the ship's reachable x, so they need no
      collide override — same as the bridge pillars.

- 2026-05-15 — **Procedural music variety.** The synthwave
  generator (`music/music_procedural.c`) had a single fixed arp
  pattern, a hard-coded eighth-note bass, three drum patterns and
  a fixed 110 BPM. Added, all chosen fresh per 16-bar section and
  still fully seed-deterministic:
    - **Arp pattern bank** — 6 patterns (`g_arp_patterns[]`),
      picked with a no-repeat re-roll. Steps gained a rest value
      (`-1`) for rhythmic gaps and extended to `fifth+octave`
      (was capped at `root+octave`). ~1/3 of sections lift the arp
      an octave (`arp_octave`).
    - **Bass rhythm bank** — 4 16-bit masks (`g_bass_patterns[]`:
      eighths, driving sixteenths, quarters, gallop) replacing the
      hard-coded pulse, plus a chance to walk to the fifth on a
      bar's last 16th.
    - **Drum bank** — grew 3 → 6 patterns.
    - **Per-run tempo** — each run picks an integer BPM in
      [100, 118]; `samples_per_16th` is now a per-run
      `music_proc_t` field, not a compile-time constant.

- 2026-05-15 — **Upstream IMU API cherry-picked.** Pulled the
  template's gyro/IMU commits (`Add IMU API`, `BF gyro api`) —
  new `include/graceloader_imu.h` (BMI270 accelerometer +
  gyroscope: `bsp_orientation_enable_*` / `bsp_orientation_get`,
  axis documentation) and the updated `fakelib/liball.so` that
  exports the orientation symbols. The upstream `Fix readme`
  commit was deliberately *not* taken — it only touches
  `README.md`, and our README is a local game-specific version
  that must stay as-is.

- 2026-05-15 — **Gyro tilt steering wired to the Controls
  toggle.** The "Gyroscope" checkbox added in the 2026-05-15 menu
  redesign (previously stored-but-inert) now drives motion
  steering.
    - **Accelerometer, not the rate gyro.** Despite the menu
      label, the signal is `accel_y` from the BMI270
      accelerometer. Both control styles the player asked for —
      holding the device upright and rolling it like a steering
      wheel, or holding it flat and tipping it like a marble game
      — are *absolute-tilt* gestures, and gravity is a drift-free
      tilt reference (a rate gyro would need integration and would
      drift). The device's +Y axis (right edge → left edge)
      captures the lateral gravity component in *both* poses, so a
      single signal serves both: `accel_y > 0` ⇒ steer right in
      either pose. No mode switch.
    - **Pipeline.** `input_init()` enables the accelerometer once
      at startup (non-fatal on failure). New `gyro_steering()` in
      `input.c` low-pass-filters `accel_y` (`GYRO_FILTER_ALPHA`),
      applies a deadzone (`GYRO_DEADZONE_ACCEL`, ~4°) and scales
      to full lock at `GYRO_FULL_TILT_ACCEL` (~27°). Returns 0
      when the toggle is off, so key-only players are unaffected.
    - **Analog steering.** `input_steering()` now returns `float`
      in [-1,+1] (was `int` ∈ {-1,0,+1}); `game_step()` takes a
      `float steer`. A held key still locks to ±1 and overrides
      tilt (digital wins); the gyro fills in the proportional
      in-between. Endpoints are bit-identical to the old int path,
      so the **max turn rate is unchanged** — full tilt ≡ a held
      key ≡ the old ±1.
    - On-device tuning knobs if needed: the three `GYRO_*`
      constants in `input.c`, and the sign of `gyro_steering()`'s
      final return if the steering direction turns out inverted.

- 2026-05-15 — **Crash explosion + stall hold-out.** The end-of-run
  transition no longer jumps straight from PLAYING to GAME_OVER.
  Two intermediate hold states make the run's end legible.
    - **`APP_STATE_CRASHING`** — on a head-on crash the ship is
      replaced by a 56-particle spark shower (`game_crash_burst` /
      `game_crash_tick` / `game_draw_crash_sparks` in `game.c`; the
      `crash_spark_t` pool — ~1.3 KB — lives in `game_state_t`).
      Sparks fly outward from the ship's projected position with
      random speed/direction, arc under gravity, and fade — each
      streak shrinks and cools hot-yellow → red-ember. The world
      keeps scrolling at the crash-moment speed (wreck momentum);
      input is ignored. The state ends when the sparks burn out
      (`game_crash_tick` going false — longest spark ≈0.6 s, ≈ the
      ~0.5 s crash SFX; `CRASH_ANIM_SECONDS` 0.70 is a dt guard),
      then GAME_OVER. The crashed ship is not redrawn under the
      game-over panel.
    - **`APP_STATE_STALL_OUT`** — when the ship coasts to a halt
      (shadow stall / sunset) the frozen scene is held for
      `STALL_HOLD_SECONDS` (1.5 s), ship still visible, input
      ignored, so the player registers the stall before the panel.
      No explosion SFX — the crash sound is crash-only now.
    - **End-cause fix.** PLAYING used to collapse crash + stall
      into one `head_on` signal, and GAME_OVER always passed
      `head_on=true` to `commit_run_end()` — so *every* run was
      saved as `SAVE_END_CRASH` (stalls/sunsets mis-recorded in the
      stats). The signals are now split (`crashed` / `stalled`);
      a file-static `s_run_was_crash` carries the real cause into
      GAME_OVER, so stalls save as `SAVE_END_STALL` and sunsets as
      `SAVE_END_SUNSET`.
    - On-device tuning knobs: the `CRASH_SPARK_*` constants in
      `game.c` (count/life/speed/gravity/length) and
      `STALL_HOLD_SECONDS` in `main.c`.

- 2026-05-15 — **Day-rollover detection + single session-date
  snapshot.** Closes the last pure-Phase-8 gap and fixes a
  mid-session-correctness hole.
    - **Rollover.** New `save_apply_day_rollover()` in `main.c`,
      run once on a freshly loaded slot: if the calendar day has
      moved past the save's `meta.last_seen_date`, it stores the
      new date and clears the `daily.daily_done_{1,2,3}pt`
      challenge-completion booleans. The reset is in-memory only —
      the next run-end commit persists it and the check is
      idempotent across boots, so no boot-time flash write is
      forced (which also avoids a spurious `last_played_unix`
      bump). NB: the `daily_done_*` flags are still only *written*
      by the not-yet-built Phase 11 challenge system, so this is
      correct-but-inert plumbing until challenges land.
    - **Session-date snapshot.** The RTC's *current* time is now
      read in exactly one place — `capture_session_date()`, called
      once in `app_main` — caching the day into a file-static
      `s_session_date` (`yyyymmdd`, 0 if the RTC was unset at
      boot). `derive_daily_seed()` (previously read `time()` every
      `start_run`) and `save_apply_day_rollover()` (previously read
      `time()` at slot-load) both now read that snapshot. So "today"
      is frozen for the whole session: the daily world can't
      reshuffle between runs, and the rollover can't fire mid-run,
      if the player crosses midnight. Crossing midnight takes
      effect only at the next launch — correct for a daily game.
      Future challenge code must also read `s_session_date`, never
      the RTC. (The lone remaining `localtime_r`, `format_unix` at
      `main.c`, formats a *stored* `last_played_unix` for display —
      not a current-day read — and is correctly left alone.)

- 2026-05-15 — **Phase 8 closed; meta-progression re-scoped to
  Phase 11.** The original plan filed the meta-progression layer
  (daily challenge system, level/points, unlocks) partly under
  Phase 8, but the actual code splits along a different line: the
  daily *seed* and the save-file *persistence* are self-contained
  and done, while the challenge/level/unlock *logic* is an
  unstarted module (`meta.c`) that the plan also describes under
  Phase 11. Phase 8 contained a piece — the `daily_done_*`
  rollover — that is inert without Phase 11's challenge system.
  Decision: Phase 8 is marked ✅ done in its true scope (daily +
  custom seed + persistence — playable end-to-end with persistent
  per-slot scores, the MVP bar), and *all* meta-progression work
  moves wholesale to Phase 11, whose status row was expanded to
  say so. The `save_data_t` already carries every meta field
  (`level`, `points`, 24× `unlock_*`, `daily_done_*`); Phase 11
  fills in the code that reads and writes them. No code changed —
  this is a planning/tracking correction only.

- 2026-05-15 — **Phase 9 sub-phases + 9.1 vertical-system design.**
  Phase 9 (pickups & attachments) is split into five runnable
  sub-phases — 9.1 vertical system + jump + ramps, 9.2 shield,
  9.3 checkpoint, 9.4 attachment slots + magnet, 9.5 battery — and
  built **ungated**: pickups always spawn, attachments are always
  equippable, the `unlock_*` flags are wired later in Phase 11.
  This entry is the full design for **9.1**, which adds the ship's
  vertical dimension. Everything below is a *design decision*, not
  yet implemented.
    - **Vertical model.** The ship gains `ship_y` + `ship_vy`;
      `game_step` integrates a gravity-driven arc. The jump pickup
      injects `vy` explicitly on the use-button press; ramps do
      not inject anything (see below). Camera stays Y-fixed for
      now — the ship visibly rises in frame. Revisiting that later
      is a mechanical change: add a `cam_y` parallel to `cam_x`
      and thread it through `render_project`.
    - **Shadow becomes a ray test.** Today `g->in_shadow` is *not*
      geometry — `main.c` samples the floor pixel under the ship
      and checks if it's shadow-coloured, which only works because
      the ship is glued to the floor. That sampler is replaced by
      a shadow ray: from the ship centre toward the sun, first
      obstacle hit ⇒ shadowed. The sun direction is **back-tracked
      from the existing shadow-length math**, not the painted sun
      (which is just a UI indicator): rendered shadows are
      `caster_height × factor` long, so the light direction toward
      the sun is `(0, 1, factor)` — zero lateral, `+y`, `+z`, with
      `factor` the same `GAME_SHADOW_LEN_FACTOR_*`/`sun_y` value
      the renderer uses. Gameplay shadow and visible shadows
      therefore agree by construction. Ray-vs-AABB (obstacles are
      axis-aligned boxes — exact and cheap; no triangle
      decomposition); ramps use their AABB. The test folds into
      `game_collide`'s existing obstacle loop — no extra pool
      traversal. Post-sunset stays a synchronous `in_shadow =
      true`. The rendered floor-shadow quads stay, now purely
      cosmetic and decoupled from gameplay. This also handles
      airborne-under-an-overhang (ray hits the bridge span ⇒
      shadowed) and platform-riding uniformly — one test, every
      case.
    - **Collision becomes Y-aware.** `game_collide`'s x-z AABB
      gains a third interval test against each obstacle's
      `[y_base, y_base + height]`; no Y-overlap ⇒ `continue`.
      Fly-over, fly-under and floating mid-air obstacles fall out
      for free. This **retires the bridge-span `span_collide`
      `IGNORE` hack** — it exists only because "default collision
      is x-z only"; once collision sees Y, the elevated span
      simply doesn't overlap a grounded ship (and becomes
      landable, see below). Swept collision is z-only today
      (anti-tunnel); a fast vertical jump may need swept-Y to not
      tunnel thin platforms.
    - **Landing on / riding tops.** A "came-from-above" test
      mirrors the existing "came_from_ahead" z-classifier: ship
      belly above an obstacle's top face last frame, descending
      (`vy < 0`), now x-z over it ⇒ **land** (snap `ship_y` to the
      top, `vy = 0`). An at-altitude contact with a front/side
      face is still a crash. Ground state becomes a small enum —
      `GROUND_FLOOR` / `GROUND_OBJECT(id)` / `AIRBORNE`; riding off
      the platform's back edge (it scrolls past) or its side
      (steering) drops back to `AIRBORNE`. All solid obstacle tops
      are landable uniformly — cubes, blocks, gate walls, bridge
      spans.
    - **Ramps — emergent launch.** Ramps are visible wedge
      obstacles (the first non-cuboid render) of varying width and
      steepness. Launch is emergent, not injected: a ramp is a
      sloped platform, the ship rides its slope and is already
      climbing at `vy = ship_speed_z × slope`; when the top lip
      ends, the ship simply keeps that `vy` and gravity takes
      over. Steeper ramp and faster ship both yield more air, for
      free. Ramps therefore depend on the riding/landing code,
      which is why jump and ramps are one sub-phase.
    - **Border walls — collision → clamp.** `OBSTACLE_KIND_WALL`
      (the track-edge side walls) leaves the collision pass
      entirely — `game_collide` skips it. The walls stay in the
      pool only for rendering. The lateral clamp already in
      `game_step` (`SHIP_X_MIN/MAX_WORLD`) becomes the boundary:
      when the clamp truncates outward motion this frame, set
      `scrape_left/right` (same scrape SFX + wingtip sparks).
      That fires only while the player pushes into the edge and is
      X-only, so it works identically at any altitude — the border
      is effectively an infinite-height wall the ship can never
      leave or jump over. **To verify before implementing:**
      `OBSTACLE_KIND_WALL` is used *only* for border walls — gate
      slabs / in-track blocks must be `OBSTACLE_KIND_CUBE`, or
      skipping `WALL` would make them pass-through.

- 2026-05-15 — **Phase 9.1 implementation: milestones a / a.5 / b
  landed.** The vertical system is built as build-verifiable
  sub-milestones (see the design entry above for the full spec).
  Done so far:
    - **9.1a — vertical state + gravity + jump trigger.**
      `game_state_t` gained `ship_y` (altitude above the rest
      height) + `ship_vy`. `game_step` integrates a gravity arc;
      `game_jump()` injects `GAME_JUMP_SPEED` when grounded (no
      double-jump). The use-item button (Space) triggers it during
      PLAYING — ungated until the 9.1f inventory gate. Ship mesh
      and crash-spark burst render at the live altitude. After an
      on-device test the arc was retuned much floatier —
      `GAME_GRAVITY` 11→3.5, `GAME_JUMP_SPEED` 6→3.3 (~1.9 s
      airborne, ~1.6 u peak) so the rise/fall is easy to read.
    - **9.1a.5 — moving camera + dedicated camera global.** A new
      sub-milestone — the design had filed a Y-fixed camera, but
      on-device the fixed camera made jumps hard to judge and would
      hide raised platforms, so the user opted to do camera Y now.
      New `render_camera_t` global in render.c (`render_set_camera()`
      / `render_camera()`); `render_project` reads it and lost its
      `cam_x` parameter (~37 call sites updated). `cam_y` lives
      ONLY in the global — never threaded. The floor grid
      (`synthwave_step_lines`) gained a `cam_y` arg and projects
      with `horizon + FLOOR_F·cam_y/z` — the exact render_project
      ground-plane formula — so the grid tracks the camera height.
      The cached sky/sun/mountains backdrop is untouched (the
      horizon is translation-invariant). main.c publishes the
      camera each frame: `cam_y = RENDER_CAM_Y + GAME_CAM_Y_FOLLOW
      · ship_y`. After an on-device test `GAME_CAM_Y_FOLLOW` was
      set to 1.0 — the camera holds a constant height above the
      ship, ship at a stable screen position. (cam_x stays a
      threaded parameter where it is used for face-visibility;
      only cam_y was unified into the global. The now-unused cam_x
      in `span_draw` / `span_shadow` is `(void)`-cast.)
    - **9.1b — Y-aware collision.** `game_collide` gained a third
      interval test: the ship box `[SHIP_BASE_Y+ship_y, +SHIP_`
      `COLLISION_HEIGHT]` (0.30, from the ship mesh's local-y
      extent) against each obstacle's `[y_base, y_base+height]`.
      No Y-overlap ⇒ continue — fly-over, fly-under and floating
      obstacles all work. Jumping over a Tri / booster now misses
      it (no Y-overlap = no pickup) — correct for a jump game; the
      magnet (9.4) is what reaches flown-over pickups. The bridge
      `span_collide` IGNORE hack was retired. **Bug found + fixed
      on-device:** retiring `span_collide` exposed that the span's
      `o->y_base` was 0 — its elevation lived only in the custom
      `span_draw` renderer (a constant), which the IGNORE hack had
      masked. With Y-aware collision the span became a full-width
      *ground-level* phantom box and crashed the ship flying under
      bridges. Fixed by setting `span->y_base = BRIDGE_SPAN_Y_BASE`
      so the obstacle AABB matches the rendered geometry.
  Still pending in 9.1: c (landing on tops), d (shadow ray),
  e (border-wall clamp), f (jump booster + inventory + HUD),
  g (ramp object), h (simple_platform area).

- 2026-05-16 — **Phase 9.1 implementation: milestones c / d / e / h
  landed.** Continues the vertical-system build.
    - **9.1c — landing on / riding obstacle tops.** `game_step`
      recomputes a *support surface* every frame — the highest
      landable `KIND_CUBE` top under the ship's x-z footprint, or
      the floor. A descending ship snaps onto the support (that
      snap is the landing); `ship_grounded` is set and `game_jump`
      reads it (jump from a platform top — still no double-jump).
      Recompute-every-frame means riding off an edge (the platform
      scrolls past, or steer off the side) just drops the support
      and the ship falls — the design's stored `GROUND_OBJECT(id)`
      enum proved unnecessary. `game_collide` gained a top-contact
      rule: ship belly at/above an obstacle top (within
      `GAME_LAND_EPS`) ⇒ a ride/land, not a crash. `game_step` now
      takes the world for the support scan.
    - **9.1d — shadow ray.** The gameplay `in_shadow` flag is now
      geometry: a ray from the ship centre toward the sun,
      direction `(0, 1, factor)` back-tracked from the renderer's
      shadow-length math. Ray-vs-AABB against each `KIND_CUBE`,
      folded into `game_collide`'s existing obstacle loop (no
      second traversal); first hit ⇒ shadowed. The old floor-pixel
      sampler in `main.c` is deleted — the ray works at any
      altitude (airborne under an overhang ⇒ shadowed) with no
      render dependency or one-frame lag. `game_after_collide`
      keeps only the post-sunset override. The rendered
      floor-shadow quads are now purely cosmetic.
    - **9.1e — border walls become a clamp.** Verified
      `OBSTACLE_KIND_WALL` is spawned only by `wall.c` (the border
      side walls). `game_collide` skips `KIND_WALL` entirely
      (collision, shadow ray, support scan); the dead `WALL`
      dispatch case was removed. Walls stay in the pool only for
      rendering. The lateral clamp in `game_step` IS the wall —
      infinite-height by construction. A wall-scrape is derived at
      `game_collide`'s tail: ship pinned at a boundary AND banking
      into it ⇒ scrape flag (same SFX + sparks); banking away
      clears it. **Edge-aware clamp (on-device fix):** the clamp
      first pinned the ship's *centre* to ±5.0, so half the
      0.28-wide hull sat inside the wall. It now clamps the ship's
      *edge* — new `ship_lateral_half_w()` =
      `SHIP_COLLISION_HALF_W · cos(bank · MAX_BANK_RAD)`: a full
      0.28 when level, shrinking with bank as the wing tips roll
      out of the horizontal plane. Applied to all three
      lateral-bound sites (the `game_step` clamp, the
      obstacle-scrape re-clamp, the wall-scrape detection).
    - **9.1h — simple_platform area.** New
      `AREA_TYPE_SIMPLE_PLATFORM` (`areas/simple_platform.{c,h}`):
      a 50 u lead-in gap (reserved for the ramp — 9.1g), then
      10-16 contiguous wall-segment-long blocks forming one
      elevated platform (`y_base` 0.75, top 1.25 — the ship passes
      under at ground level, lands on top after a jump), then a
      30 u trailing gap. Each block carries a Tri on its top face;
      one becomes a booster if the stage scheduler owes one.
      `render_pickup_pyramid` / `render_booster_icosahedron` were
      made `y_base`-aware so elevated pickups render on the
      platform. Stage 2+; the Tab debug key now forces this area.

- 2026-05-16 — **Phase 9.1 complete: milestones f / g landed.**
  The final two sub-milestones close out the vertical system.
    - **9.1f — jump booster + inventory + HUD.** New
      `objects/jump_booster.{c,h}` — a red rotating octahedron
      (6 verts / 8 faces) drawn via a `draw` callback, with
      per-face lighting + a true sign-parity checkerboard two-tone
      (a flat `f & 1` clumped the shading — fixed). `game_state_t`
      gained `jump_charges`; collecting a jump booster
      (`OBSTACLE_KIND_PICKUP_JUMP`) grants +1 (capped at
      `GAME_JUMP_CHARGE_MAX` = 3), counts `pickups_jump`, and reuses
      the `just_picked_up_booster` audio flag so it plays the same
      ding as a speed booster. `game_jump()` now requires and
      spends a charge — jumping is a consumable, no longer free.
      HUD `draw_jump_inventory()` shows one red diamond per charge,
      bottom-right, sized to match the boost indicator. One jump
      booster spawns in the start-area lead-in; in-run they spawn
      in the between-stage rest areas from stage 3 onward.
    - **9.1g — ramp object + emergent launch.** New
      `objects/ramp.{c,h}` — a dull-yellow wedge (triangular
      prism) with a bright-yellow wireframe, custom `draw` callback
      (sloped top + the camera-side face; an edge-based
      side-visibility test, after a centre-based one wrongly drew a
      face while the ship rode the ramp). `game_step`'s vertical
      model was reworked to the canonical platformer form (gravity
      always → integrate → resolve); the support scan now also
      handles `KIND_RAMP`, sampling the wedge's sloped surface and
      a `support_slope`. Riding a ramp sets `ship_vy =
      ship_speed_z · slope`, so the instant the ramp ends the ship
      keeps that climb velocity and arcs into the air — the launch
      is emergent, no injected impulse. `GAME_RAMP_STEP_UP` tuning
      constant added. The ramp is spawned at the head of the
      `simple_platform` lead-in (the gap 9.1h reserved). **Open
      tuning item, deliberately parked:** the ramp→platform arc
      alignment (`SP_RAMP_HALF_D` / `SP_RAMP_RISE` / `SP_LEAD_Z` in
      `simple_platform.c`) — left at first-pass values for now.
  **Phase 9.1 (the vertical system) is complete** — ship altitude,
  gravity, jumps, Y-aware collision, landing/riding obstacle tops,
  the geometric shadow ray, the moving camera, border-wall clamp,
  the jump booster, ramps, and the simple_platform test area.

- 2026-05-19 — **Engine-hum default off; elevated-platform fixes;
  current performance baseline.**
    - Engine-hum SFX now defaults **off** when no value is stored in
      NVS (`audio_settings.c` — `s_hum_on = false`).
    - `simple_platform` block elevation doubled — `SP_BLOCK_Y_BASE`
      0.75 → 1.5, so the block top sits at world-y 2.0 (above a plain
      jump's ~1.78 reach; the launch ramp is now the intended way up).
    - The default cube renderer gained a **bottom face**. It was
      written assuming cubes sit at ground level below the camera and
      only drew front/left/right/top; with elevated platform blocks
      the ship now drives *under* them, so a `show_bottom`
      (`render_camera().y < yB`) face + its wireframe edges were
      added. Fixes the see-through underside and pickups-on-tops
      bleeding through the missing face.
    - **Current on-device performance baseline** (gameplay, ~25.5 FPS,
      ~39 ms frame, fully CPU-bound — `vsync=0`):

      | Phase  | ms    | % frame | Notes |
      |--------|-------|---------|-------|
      | in     | 0.07  |  <1%    | input drain |
      | phys   | 0.65  |   2%    | game_step/collide/world_advance |
      | bgkick | 10.4  |  27%    | backdrop — PSRAM-bandwidth-bound |
      | bgflr  | 14.8  |  38%    | floor fill + lane lines |
      | bgwait | 0.01  |  <1%    | |
      | obs    | 6.16  |  16%    | 3D obstacle pass (direct_565) |
      | fgrest | 6.35  |  16%    | ship + sparks + HUD |
      | blit   | 0.67  |   2%    | DMA queue |
      | **FPS**| —     | —       | **~25.5** |

      Note: the `obs=46 ms` figure quoted in the older RGB565
      scoreboard entries is **stale** — the `direct_565` rewrite cut
      the obstacle pass to ~6 ms. The frame is now dominated by
      `bgflr` + `bgkick` (~25 ms, 64%).
    - **Renderer-correctness discussion (no code yet).** Per-object
      painter's sort by centre `z_world` can't resolve a pickup
      resting on a block when viewed from below (vertical separation
      the z-only key ignores). Options weighed: per-facet
      squared-distance-to-camera sort (near-zero cost, fixes the
      stacked case, still sort-dependent); a 1-bit SRAM coverage mask
      with front-to-back draw (kills overdraw + needs no PSRAM depth
      buffer, still sort-dependent); a per-pixel z-buffer (bulletproof,
      keeps edge lines via a depth-biased no-z-write line pass,
      ~+4–8 ms ⇒ ~21–23 FPS); a raycaster (rejected — compute-bound,
      and exact per-pixel intersection buys nothing for flat-shaded
      polygons). User wants full correctness ⇒ leaning z-buffer.
      Decision/implementation deferred.

- 2026-05-19 — **Z-buffer renderer + geometry-emitter pipeline.**
  Replaced the per-object painter's algorithm with a per-pixel
  depth-buffered scene. Built on branch `z_buffer`.
    - **New module `scene.{c,h}`.** Owns a `uint16_t` depth buffer
      (384 000 px, 768 KB PSRAM) indexed identically to the
      framebuffer. Depth is scaled reciprocal-z (1/z; larger =
      nearer; cleared to 0 each frame in `scene_begin`).
      `scene_tri()` projects + rasterizes a world-space triangle
      *immediately* with a per-pixel depth test + write — the
      z-buffer makes triangle draw order irrelevant, so no sort and
      no triangle buffer. `scene_line()` defers edges into a buffer;
      `scene_flush()` rasterizes them last, depth-tested with a
      ×1.02 bias (an edge beats the coplanar face it outlines but
      still loses to nearer geometry) and **no** depth write.
      Near-plane handling is centralised here: each vertex's z is
      clamped to `RENDER_NEAR_CLIP_Z`, wholly-behind geometry
      dropped.
    - **Geometry emitters.** `render_obstacles` → `render_submit_-
      obstacles`: the z-sort is gone; cube / pyramid / icosahedron
      became emitters that submit world-space tris+lines. The four
      custom `draw` callbacks (jump_booster, flipping_cube, ramp,
      and — removed entirely — bridge span) became `emit` callbacks;
      `obstacle_t.draw` → `obstacle_t.emit`, signature now just
      `(obstacle_t const*)` (camera read from `render_camera()`).
      The bridge span's custom renderer was deleted outright — its
      `y_base` already carries its elevation and the y_base-aware
      `emit_cube` renders it correctly. Per-face camera-side culls
      are kept purely as a speed optimisation.
    - **Ship is now part of the world.** `game_draw_ship` →
      `game_submit_ship`: the ship mesh submits into the scene like
      any obstacle, so it is depth-tested against geometry it flies
      under / behind / alongside (no more separate 2D pass).
    - **main.c** — `scene_init()` at boot; the six `render_obstacles`
      + `game_draw_ship` sites collapsed to one `render_run_scene()`
      helper (`scene_begin` → submit obstacles → optional ship →
      `scene_flush`). `obs` now profiles the whole z-buffered pass.
    - Visual style preserved: same palettes, flat shading, cyan
      wireframe, near-clip behaviour. What changed is that
      visibility is per-pixel correct — stacked / straddling /
      mutually-overlapping geometry all resolve without draw-order
      reasoning, and the obstacle code got simpler as a result.
    - Builds clean (`make clean build`, `make verify`). On-device
      FPS impact to be measured.

- 2026-05-19 — **Z-buffer first measurement + optimisation pass;
  debug godmode.**
    - **First on-device measurement:** the z-buffer pushed `obs`
      6 → ~30 ms (FPS ~25 → ~16). Two causes: the 768 KB depth-clear
      `memset` to PSRAM each frame, and the fill inner loop losing
      its near-`memset` fast path (per pixel: float depth multiply,
      clamp, cast, compare, two buffer writes).
    - **Killed the clear — frame-stamp plane.** The frame-ID-in-the-
      depth-word trick was rejected: it widens every per-pixel depth
      access, and since the 3D scene covers most of the screen that
      costs more than the clear it removes. Instead a parallel 8-bit
      stamp plane (`s_stamp`, 384 KB PSRAM) records the frame tag
      that last wrote each pixel's depth; a depth counts only when
      its stamp == the current frame, else it reads as far.
      `scene_begin` just increments the tag (skipping 0) — no memset.
      Depth traffic is now proportional to drawn area, not the whole
      screen. Stamp is 8-bit so it wraps every 256 frames — a
      worst-case 1-pixel/1-frame mis-resolve, invisible in practice.
    - **Tightened the fill loop.** The depth plane coefficients are
      pre-scaled into encoded-uint16 units, so `scene_vrun` does one
      float add per pixel — no multiply, no high-end clamp (the
      near-clip guarantees the range), just a low-end overshoot
      guard.
    - **Debug godmode (G key).** Toggles `s_godmode`; while on, the
      crash and stall end-of-run signals are forced false in the
      physics pass, so the ship coasts through head-on hits and never
      stalls — lets the run be slowed down / inspected. New slot-4
      HUD readout below `sun=`: `god=ON/off x=.. y=..` (cyan when
      active, grey when not). `input_consume_godmode_toggle()` mirrors
      the TAB force-area debug hook.
    - **Post-optimisation measurement.** `obs` 30 → ~21 ms, FPS
      ~16 → ~20 — the pass recovered ~9 ms (≈6 ms was the clear,
      ≈3 ms the tighter fill loop). Scoreboard across the three
      stages of the z-buffer work:

      | Phase | pre-z-buf | z-buf v1 | z-buf v2 |
      |-------|-----------|----------|----------|
      | obs   | 6 ms      | ~30 ms   | ~21 ms   |
      | FPS   | ~25.5     | ~16      | ~20      |

      The residual ~21 ms vs the old 6 ms is the intrinsic cost of
      per-pixel correctness: every filled pixel does a depth + stamp
      read + compare (three PSRAM regions touched — fb, depth, stamp
      — vs one before), and ~1500 wireframe edges each depth-test per
      pixel. Further `obs` gains are diminishing returns; the frame
      is now bottlenecked by `bgflr` + `bgkick` (~25 ms, ~50% of the
      frame), which is the real target for any future FPS work.
      Smaller `obs` levers left, if wanted later: interleave depth +
      stamp into one buffer for cache locality, move the line buffer
      to internal SRAM, front-to-back order to skip color writes on
      depth-rejected pixels.

---


- 2026-05-19 — **Phase 9.2 — shield pickup.** Single-use shield that
  absorbs a head-on crash. Built on branch `z_buffer`'s successor work
  (committed to `main`).
    - **Shield object** — `objects/shield.{c,h}`: a violet hexagonal
      plate (`#B060FF` caps, `#6028A0` rim, `#D8B0FF` wireframe),
      slowly spinning around Y on the shared booster cadence,
      emit-based so it is depth-tested in the scene. Shape + colour
      picked to stay distinct from the blue Tri pyramid, green speed
      icosahedron and red jump octahedron.
    - **Generalised owed-pickup API.** `area_state_t` gained an
      `owed_pickups[AREA_OWED_COUNT]` array (enum `AREA_OWED_SHIELD`,
      `AREA_OWED_CHECKPOINT` reserved for 9.3) and a shared helper
      `world_place_pickup(w, a, x, z)` that every area's Tri-spawn
      site now routes through — it upgrades a Tri slot to one owed
      special pickup (consumed once per slot) or spawns a plain Tri.
      The speed booster was deliberately **left as-is** on its own
      `boosters_owed` field and per-area placement (it displaces a
      big cube in pixel_field/big_blocks, rides the gate gap in
      gateways — routing it through the Tri helper would have moved
      it). `gateways`' `spawn_gate` and `dynamic_passage`'s `emit_tri`
      had the `area_state_t*` threaded in to reach the helper.
    - **Spawn schedule.** One shield per stage from
      `GAME_SHIELD_FIRST_STAGE` (= 3), due at a random point in the
      first 70% of the stage so Tri slots follow it to host it
      (`world_state_t.shield_due_at_progress`, checked in
      `world_advance` alongside the booster scheduler). The
      pre-stage-1 lead-in spawns a shield instead of a jump booster.
    - **Effect.** Collection banks a charge (`game_state.shield_-
      charges`, cap `GAME_SHIELD_CHARGE_MAX` = **1**; checkpoint will
      also cap at 1, jump stays at 3). A head-on crash spends the
      charge and opens a `GAME_SHIELD_DURATION` (4 s) invulnerability
      window (`shield_timer`): crashes are survivable during it, but
      the crash SFX + spark shower still fire on each hit, debounced
      by `GAME_SHIELD_HIT_COOLDOWN` (0.35 s). Auto-activates on the
      crash (absorbs the hit) rather than being a button press.
    - **Feedback.** `game_draw_shield` paints a spinning, gently
      pulsing violet hexagonal ring (`SHIELD_AURA_RADIUS` 90 px)
      around the ship's projected position while the window is open,
      blinking through the final second. HUD: a violet hexagon
      bottom-right, one row above the jump diamonds, via
      `draw_shield_inventory`.
    - **Phase 9.2 is complete.** Remaining in Phase 9: 9.3 checkpoint,
      9.4 attachment slots + magnet, 9.5 battery.
