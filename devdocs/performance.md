# Performance & FPS

> Frame-time profiling and the catalogue of viable optimisations. Part of the [dev docs](README.md).

## Future FPS improvements

Catalogue of viable optimisations parked at the 28 FPS plateau
(2026-05-11). Sorted roughly by expected wallclock-saved-per-unit-effort.
Estimated wins are rough — measure before committing.

### Background pipeline (`bgkick + bgflr ≈ 25 ms`)

1. **PPA FILL for the floor base** (-7 to -10 ms `bgflr`,
   medium effort). The floor's purple base rect
   (`pax_simple_rect`, 800×224 px) is currently CPU work — about
   10 ms in `bgflr`. Replace with a fourth PPA op
   (`ppa_do_fill`) covering the below-horizon region. Adds one
   more PPA op to the serial chain (~2 ms), nets ~7–8 ms saved.
   Trade-off: more PPA op ordering complexity, one more wait.

2. **PPA callback chaining** (-5 to -8 ms `bgkick`, high
   effort, risky). The current serial waits between FILL → SRM
   → BLEND cost ~5 ms of pure idle time. Submit each op
   non-blocking; in the `on_trans_done` callback (ISR context)
   submit the next op directly. Restores full PPA-vs-CPU
   parallelism. Risk: calling `ppa_do_xxx` from ISR may not be
   safe in this IDF version — needs validation. Fallback: a
   high-priority FreeRTOS task that wakes on completion and
   submits the next op.

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

### Obstacle pipeline (`obs ≈ 5 ms`)

5. **Z-buffer rasterisation** (variable, medium-high effort).
   Add a 1-byte-per-pixel depth buffer. Triangles write depth +
   color, skipping pixels behind already-written content. Big
   help if the scene ever produces significant overdraw (the
   user's expectation: 3-4× obstacle count plus pickups,
   particles, etc.). Per-pixel cost goes from "always write"
   to "test, maybe write" — wins when overdraw factor > ~2.
   Memory cost: ~384 KB for a 800×480 8-bit depth buffer.

6. **Per-cube LOD** (-1 to -2 ms `obs`, low effort). Drop
   wireframes for cubes farther than ~25 world units, and drop
   side/top faces when the cube projects to <8 px. The visual
   impact is small (those cubes are 1–4 px wide near the
   horizon) and they currently cost full per-triangle setup.

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

