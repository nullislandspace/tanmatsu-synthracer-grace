# Importing a world object from OpenSCAD

How to turn an OpenSCAD model into an in-game world object, end to end,
using `tools/object_3mf_to_header.py`. The rest-area marker
(`objects/restarea_marker.c`) is the worked example throughout.

> This is the path for **world objects** — things placed by an obstacle
> `emit` callback (pickups, scenery, markers). The **ship** has its own
> near-identical tool (`tools/ship_3mf_to_header.py`); the two could be
> merged later but are kept separate so ship changes stay isolated.

## Pipeline at a glance

```
model.scad ──(OpenSCAD F6 + Export 3MF)──▶ openscad/foo.3mf
   openscad/foo.3mf ──(object_3mf_to_header.py foo)──▶ main/objects/foo_model.h
   foo_model.h ──(#include + emit callback)──▶ main/objects/foo.c
   foo.c ──(spawn from an area)──▶ rendered in the world
```

Geometry lives entirely in the generated header. To change the model
later, re-export the 3MF and re-run the converter — **no C changes**.

---

## 1. Model it in OpenSCAD

Two conventions matter:

**Axes.** OpenSCAD is Z-up. The game is Y-up: **x = lateral, y = up,
z = forward (into the screen)**. The converter remaps
`game = (scad_x, scad_z, scad_y)` for you, so in OpenSCAD just build the
object **standing up along +Z**, with its forward direction along +Y if
that matters. The handedness flip from the axis swap is corrected
automatically (triangle winding is reversed so faces stay CCW-outward).

**Colour = part tag.** Wrap each logical part in `color("#RRGGBB")`. The
converter uses those colours **only to tag triangles into regions** — not
as the final look (region fill/outline colours are set in the config and
can be driven at runtime). Give each part a distinct, exact hex. Build the
parts as separate solids; OpenSCAD exports each top-level coloured part as
its own `<object>`/`<mesh>`, which the converter relies on.

Keep it **low-poly** — every triangle costs a transform + cull + fill each
frame, and every kept feature edge is a line. Use low `$fn`.

Example (`openscad/restarea_markers.scad`): a grey tapered hex post plus a
green sphere on top:

```scad
color("#A0A0A0") cylinder(h=20, d1=4, d2=1.5, $fn=6);   // post
color("#00FF00") translate([0,0,22.3]) sphere(d=5, $fn=7); // beacon
```

## 2. Export to 3MF

In OpenSCAD: **F6** (full Render — not just the F5 preview), confirm the
parts are where you expect, then **File → Export → Export as 3MF**. Make
sure colour export is on (a recent OpenSCAD with the Manifold backend).
Save into `openscad/`.

> If a re-export "doesn't take", you almost certainly exported a stale
> compile — re-render with F6 first. (We once chased a misplaced part for
> a while before realising the 3MF held an un-rendered version.)

## 3. Register the object in the converter

Add an entry to the `OBJECTS` dict at the top of
`tools/object_3mf_to_header.py`. Every field, using the marker as the
template:

```python
'restmark': {
    'src': 'openscad/restarea_markers.3mf',     # input
    'dst': 'main/objects/restarea_marker_model.h', # output header
    'prefix': 'RESTMARK',          # symbol/macro prefix: RESTMARK_VERTS, …
    'enum':      'restmark_region_t',
    'vert_type': 'restmark_vert_t',
    'tri_type':  'restmark_tri_t',
    'edge_type': 'restmark_edge_t',
    # OpenSCAD colour -> region; anything not listed -> default_region.
    'color_region': {'#00FF00': 'RESTMARK_REGION_BEACON'},
    'default_region': 'RESTMARK_REGION_POST',
    # Enum order (also the [region]=… initialiser order below).
    'region_order': ['RESTMARK_REGION_POST', 'RESTMARK_REGION_BEACON'],
    # Default fills (ARGB). The emit code may override at runtime.
    'region_fill':    {'RESTMARK_REGION_POST': '0xFFA0A0A0u',
                       'RESTMARK_REGION_BEACON': '0xFF00FF00u'},
    # Per-region wireframe colours.
    'region_outline': {'RESTMARK_REGION_POST': '0xFFFFFFFFu',
                       'RESTMARK_REGION_BEACON': '0xFF404040u'},
    # Which regions get an outline at all.
    'outline_regions': {'RESTMARK_REGION_POST', 'RESTMARK_REGION_BEACON'},
    'scale':   ('height', 2.0),    # see below
    'vanchor': 'base',             # see below
},
```

Field notes:

| field | meaning |
|-------|---------|
| `color_region` / `default_region` | hex → region name; unlisted colours fall to the default |
| `region_order` | enum member order; first is index 0 |
| `region_fill` / `region_outline` | default ARGB per region (the emit code can override fills, e.g. a pulse) |
| `outline_regions` | regions that get a wireframe; omit small parts you don't want outlined |
| `scale` | `('height', H)` scales the model so it's `H` world-units tall; `('wingspan', W)` scales lateral extent to `W` |
| `vanchor` | `'base'` puts the model's lowest point at the origin (stands up from where it's placed, e.g. a wall top); `'center'` centres it vertically |

Lateral (x) and forward (z) are always centred on the bounding box.
`FEATURE_ANGLE_DEG` (module-level, default 18°) controls the outline:
an edge is kept only where its two faces meet at a crease in
`[18°, 162°]`, which drops coplanar diagonals, near-flat "knife-folds",
and non-manifold part-junction edges.

## 4. Run the converter

```sh
python3 tools/object_3mf_to_header.py restmark
```

It prints a summary (vert/tri/edge counts, scale, per-region tri counts)
and writes the header. **Vertex count must be ≤ 256** (indices are
`uint8`); it asserts otherwise.

## 5. Write the object module

The generated header gives you, for prefix `P`:

- types `<vert_type>` `{float x,y,z;}`, `<tri_type>` `{uint8 a,b,c,region;}`,
  `<edge_type>` `{uint8 a,b,region;}`, and the region `enum` (+ `P_REGION_COUNT`)
- arrays `P_VERTS[]`, `P_TRIS[]`, `P_EDGES[]` and `P_*_COUNT` macros
- tables `P_REGION_FILL[]`, `P_REGION_OUTLINE[]`
- placement macros `P_SCALE`, `P_Y_OFFSET`, `P_Z_OFFSET` (tune these in the
  header without re-running the converter)

Write `main/objects/foo.c` with an `emit` callback. The pattern (see
`restarea_marker.c` in full):

```c
static void foo_emit(obstacle_t const* o) {
    if (o->z_world < RENDER_NEAR_CLIP_Z) return;          // near-clip guard

    // 1. Transform every model vertex to world space.
    float const s = FOO_SCALE;
    float wx[FOO_VERT_COUNT], wy[FOO_VERT_COUNT], wz[FOO_VERT_COUNT];
    for (size_t i = 0; i < FOO_VERT_COUNT; i++) {
        foo_vert_t const* v = &FOO_VERTS[i];
        wx[i] = o->x_world + v->x * s;
        wy[i] = o->y_base  + v->y * s + FOO_Y_OFFSET;   // base-anchored
        wz[i] = o->z_world + v->z * s + FOO_Z_OFFSET;
    }

    render_camera_t const cam = render_camera();
    for (size_t i = 0; i < FOO_TRI_COUNT; i++) {
        foo_tri_t const* t = &FOO_TRIS[i];
        // 2. CCW-outward normal, then back-face cull vs camera (cam.x,cam.y,0).
        // 3. Pick the fill: FOO_REGION_FILL[t->region], lit per-face or flat.
        scene_tri(/* the three world verts */, col);
    }
    for (size_t i = 0; i < FOO_EDGE_COUNT; i++) {
        foo_edge_t const* e = &FOO_EDGES[i];
        scene_line(/* e->a, e->b world verts */, FOO_REGION_OUTLINE[e->region]);
    }
}
```

Conventions worth copying from the marker / checkpoint:

- **Back-face cull** with the cross-product normal and the face→camera
  vector (`dot(n, cam - faceCentre) <= 0` ⇒ skip). Camera sits at
  `(cam.x, cam.y, 0)`.
- **Per-face lighting** on solid surfaces: `tint = 0.55 + 0.45*max(0, dot(n̂, L))`
  with the shared light `L = (-0.4, 0.7, -0.6)`, applied via a
  channel-scale of the fill. Draw "light"/emissive parts **flat** instead.
- **Time-based effects** (the marker's pulse) read `esp_timer_get_time()`;
  keep them in the emit, off the model.
- Emit **world-space** geometry only — never project or rasterise; the
  z-buffer in `scene.c` resolves visibility.

Add a spawn helper that places it as an obstacle with a custom emit:

```c
obstacle_t* m = obstacle_spawn(w, OBSTACLE_KIND_CUBE, x, z,
                               half_w, half_d, height, /* fallback colours */ …);
if (m) { m->y_base = WALL_HEIGHT; m->emit = foo_emit; }
```

`obstacle_spawn`'s colours are just fallbacks (the emit overrides
rendering); the footprint matters only for collision/shadow, so place
cosmetic objects outside the ship's reachable x and they never collide.

## 6. Wire it in and build

- Add `main/objects/foo.c` to `APP_SOURCES` in `CMakeLists.txt`.
- Call your spawn helper from the relevant area (the marker is spawned by
  `area_rest_tick` in `areas/rest.c`).
- `make build` then `make verify` from the project root (never source the
  IDF export scripts; the Makefile handles env). `make verify` must report
  "All symbols satisfied".

## Gotchas

- **Each part must be a separate coloured solid.** The converter merges
  the per-mesh vertex lists with index offsets; a single flattened parse
  would mis-index later parts. (This was a real bug — multi-mesh handling
  is the whole reason a part can land in the wrong place.)
- **No build-item transforms.** OpenSCAD exports identity transforms, so
  the converter ignores them. If a future model carries transforms,
  they'd need applying.
- **≤ 256 vertices** (uint8 indices). Keep models low-poly anyway.
- **Outline noise.** Tiny parts read badly with full wireframes — either
  leave them out of `outline_regions` or give them a muted outline colour.
- **z-fighting.** Parts that sit flush on another surface (the beacon on
  its post, an indicator on a panel) can flicker; nudge the part up
  slightly in the model if so.
- **Performance.** Each instance is (visible tris) fills + (kept edges)
  lines per frame. Watch FPS when many instances are on screen at once;
  lower `$fn` or trim `outline_regions` if needed.

See also the 2026-05-22 ship and rest-area-marker entries in
`decisions-log.md`, and the imported-mesh notes in `architecture.md`.
