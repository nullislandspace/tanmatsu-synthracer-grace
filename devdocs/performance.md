# Performance & FPS

> Frame-time profiling and the catalogue of viable optimisations. Part of the [dev docs](README.md).

## Landed (2026-05-24 optimisation pass)

Normal play went ~20 → **27–35 FPS**; a deliberate banner-spam stress scene
sits at ~11 FPS, **geometry/fill-bound** (large overhead banners = huge
triangle fill area). What changed:

- **`se_scene` prepare/rasterize split + SRAM geometry lists.** `scene_render`
  split into `scene_prepare()` (cull+order, no framebuffer) and
  `scene_rasterize()`; the deferred tri/line lists moved to internal SRAM. The
  geometry prepare (`emit`) now runs during the PPA backdrop DMA without bus
  contention.
- **depth+stamp fold.** The z-buffer + per-pixel frame stamp share one
  `uint32` cell → ~17 % off triangle raster, ~11 % off lines (the rasterizer is
  PSRAM-latency-bound; the win is fewer cache-line touches).
- **`se_ppa` is now an ordered job queue + pump task** (item #2 below, done the
  *safe* way — submission in a task, not the ISR). Each submit is a non-blocking
  enqueue tagged with a `job_id`; the pump runs jobs in submission order. This
  also removed the cross-client ordering footgun and the per-client queue-depth
  bug. **#1 (PPA floor-base FILL) landed** as part of this — the floor base is
  a FILL job that hides under the prepare.
- **Sun shrunk to its ~212 px bbox** via `se_ppa_blit_rect` (sprite blit,
  horizon-clipped) — ~73 % less SRM, and it fixed a below-horizon overdraw.
- **`depth_order` (early-z) measured a wash** even in heavy scenes (the qsort
  ≈ the early-z saving); left on by default only as headroom.

The remaining frame cost is PSRAM pixel writes — `rast` (scene rasterize) and
the floor shadows. The only big lever left is **content/LOD** (item #6) — fewer
/ smaller filled triangles, esp. distant or large-area geometry.

## Future FPS improvements

Catalogue of the still-open ideas. Sorted roughly by expected
wallclock-saved-per-unit-effort. Estimated wins are rough — measure first.

### Background pipeline

1. ✅ **DONE (2026-05-24): PPA FILL for the floor base.** The floor's purple
   base rect is now a `se_ppa_fill` job (`backdrop_submit_fill_floor`) instead
   of a CPU `pax_simple_rect`, freeing that CPU time.

2. ✅ **DONE (2026-05-24): PPA submission off the critical path.** Implemented
   as the safe fallback, not ISR chaining: a dedicated **pump task** owns
   `ppa_do_*` (task context), woken by the completion ISR. The render task just
   enqueues jobs and `se_ppa_wait_job()`s. (ISR-context submit was confirmed
   unsafe — `ppa_do_*` takes blocking locks.)

3. **Specialised direct_565 horizontal/vertical line fast
   paths** (-1 to -2 ms `bgflr`, low effort). `direct_565_line`
   uses a general Bresenham; for axis-aligned lines (the 10
   horizontal floor stripes, and many short obstacle wireframe
   edges) we can skip the branching and just stride-fill. The
   horizontal stripes especially — they're 800 px wide and
   currently pay full Bresenham overhead per pixel.

4. **Tighten the vertical-lane-line distribution** (-1 ms
   `bgflr`, low effort). The 11 lane lines from the vanishing
   point are weighted toward the near-vertical case (long pixel
   runs). Replacing the Bresenham with a direct pinhole-projected
   integer stride per row would skip the branching cost on
   every pixel. Small but cheap to do.

### Obstacle pipeline (`rast` — the dominant bucket in heavy scenes)

5. ✅ **DONE: Z-buffer rasterisation.** The `se_scene` renderer is per-pixel
   z-buffered (reciprocal-z depth + a per-pixel frame stamp, since folded into
   one `uint32` cell — see the Landed section). The opt-in front-to-back
   `depth_order` early-z pass exists too, but measured a wash in practice.

6. **Per-object LOD** (the recommended next lever, game-side). `rast` is
   fill-area-bound: large/near geometry dominates. Drop wireframe past a
   distance and drop or shrink the *fill* on small/far objects. The big
   overhead banners are the worst case — large filled triangles. Cuts `rtri`,
   `rline` and `emit` together. Lives in the game's emitters (`render.c`), not
   the engine.

7. **Tile-based screen binning** (variable, high effort).
   Split the screen into ~32×32 px tiles. During the sort pass
   compute which obstacles touch each tile. Per tile, only
   rasterise the obstacles in that tile's list. Caps per-pixel
   work at the cost of a bookkeeping pass. Pairs well with #5
   for very crowded scenes.

8. **Half-space triangle rasterisation** (correctness only,
   not perf). Current `direct_565_tri` uses ceil/floor edge
   snapping which can produce occasional 1-px gaps or
   double-writes at triangle edges (currently masked by the
   wireframe overlay). If we ever want a wireframe-free style,
   switch to half-space rasterisation with the top-left rule.

### Architectural

9. **Disable PSRAM cache for the framebuffer** (variable,
   experimental). The fb is mostly written, not read. The CPU
   cache on PSRAM acts as a write-back buffer for our pixel
   stores — useful for cache-friendly scanlines, less useful
   for strided writes (lane lines, etc.). An uncached fb might
   speed up some patterns. Risky — could regress others.

10. **Drop double-buffering and rely on TE-vsync** (-768 KB
    RAM, no FPS gain, possibly visual regression). If RAM ever
    gets tight we could try going back to single-buffer with
    careful vsync pacing. Tearing would likely return — the
    user already established this empirically.

11. **Raycasting / screen-space rendering** (variable, very
    high effort). Discussed 2026-05-11. Constant per-pixel cost
    regardless of scene complexity, no overdraw, clean fit for
    overlapping geometry and procedural effects. But the
    pixel × per-pixel-scene-test work is huge without spatial
    acceleration structures (BVH, grid, tile binning).
    Wireframe stylisation also doesn't fit naturally. Not the
    right pivot for the current cube-parade game; revisit if a
    future game design has heavy overdraw or volumetric content.

### Things deliberately NOT in the catalogue

- *Enlarge sun_cache to skip PPA FILL.* The FILL covers two
  jobs: filling sky around the sun, and wiping stale obstacle
  pixels that drifted above the horizon in the previous frame.
  Once sun_dy is non-zero the SRM destination doesn't cover the
  full sky region, so the FILL is required regardless of cache
  size.

- *Switch back to PAX for triangles/lines.* Confirmed: PAX's
  per-call setup is the bottleneck for high-frequency
  small-primitive workloads on this hardware. Going back loses
  the entire `obs` win.

---

