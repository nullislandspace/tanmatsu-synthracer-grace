#!/usr/bin/env python3
# Convert openscad/ship.3mf into main/objects/ship_model.h.
#
# The 3MF carries one shared vertex array plus per-triangle colour
# references (an OpenSCAD `color()` per part). We use those colours
# purely as PART TAGS, mapping each known colour to a render region so
# the ship body, the battery panel, and the four charge indicators stay
# separable in the generated header (the battery plugin will later drive
# the indicators from charge state).
#
# Pipeline: parse 3MF -> tag triangles by region -> remap SCAD axes
# (Z-up, +Y forward) to game axes (x lateral, y up, z forward) ->
# reverse winding (the axis swap flips handedness) so faces stay
# CCW-outward -> centre on the bounding box -> extract silhouette/feature
# edges (dihedral angle threshold) for the wireframe outline -> emit a C
# header. Scale and placement are left as tuneable macros in the header.
#
# Usage: tools/ship_3mf_to_header.py [openscad/ship.3mf] [main/objects/ship_model.h]

import math
import os
import sys
import xml.etree.ElementTree as ET
import zipfile

NS = {
    'c': 'http://schemas.microsoft.com/3dmanufacturing/core/2015/02',
    'm': 'http://schemas.microsoft.com/3dmanufacturing/material/2015/02',
}

# Colour (upper-case #RRGGBB, alpha stripped) -> region name. Anything
# not listed here falls through to the body. Edit this table if the
# model's part colours change.
COLOR_REGION = {
    '#0C0C0C': 'SHIP_REGION_BATTERY_PANEL',
    '#FFFFFA': 'SHIP_REGION_INDICATOR_0',
    '#FFFFFB': 'SHIP_REGION_INDICATOR_1',
    '#FFFFFC': 'SHIP_REGION_INDICATOR_2',
    '#FFFFFD': 'SHIP_REGION_INDICATOR_3',
}
DEFAULT_REGION = 'SHIP_REGION_BODY'

REGION_ORDER = [
    'SHIP_REGION_BODY',
    'SHIP_REGION_BATTERY_PANEL',
    'SHIP_REGION_INDICATOR_0',
    'SHIP_REGION_INDICATOR_1',
    'SHIP_REGION_INDICATOR_2',
    'SHIP_REGION_INDICATOR_3',
]

# Target full wingspan in world units. The model's lateral extent is
# scaled to this; matches 2 * SHIP_COLLISION_HALF_W (0.28) so the visual
# ship lines up with its collision box. Tune via SHIP_MODEL_SCALE later.
TARGET_WINGSPAN = 0.56
# Keep an outline edge when its two adjacent faces differ by more than
# this angle. Filters coplanar triangulation diagonals; keeps real
# facet ridges.
FEATURE_ANGLE_DEG = 18.0
# Only these regions get a wireframe outline. The battery panel and the
# four charge indicators are small, so outlining them just obscures the
# flat colour those parts carry (and which the battery plugin will drive),
# so only the hull body is outlined.
OUTLINE_REGIONS = {'SHIP_REGION_BODY'}


def load_3mf(path):
    with zipfile.ZipFile(path) as z:
        with z.open('3D/3dmodel.model') as f:
            root = ET.parse(f).getroot()
    # Colour group: index -> '#RRGGBB' (alpha stripped).
    colors = []
    cg = root.find('.//m:colorgroup', NS)
    if cg is not None:
        for col in cg.findall('m:color', NS):
            colors.append(col.get('color')[:7].upper())
    # OpenSCAD exports each top-level part as its OWN <object>/<mesh>, and
    # each mesh's triangle v1/v2/v3 indices are LOCAL to that mesh. Walk
    # the objects, concatenating all meshes into one vertex array and
    # offsetting each mesh's triangle indices by the running vertex count.
    # (The <build> items carry no transforms, so each mesh's local coords
    # are already world coords. All objects share the one colorgroup, so
    # p1 indexes into `colors` directly.) Flattening every <vertex> into a
    # single list instead — as a first cut did — leaves later meshes
    # pointing at the wrong (earlier-mesh) vertices.
    verts = []
    tris = []
    for obj in root.findall('.//c:object', NS):
        mesh = obj.find('c:mesh', NS)
        if mesh is None:
            continue
        obj_default = int(obj.get('pindex') or 0)
        base = len(verts)
        for v in mesh.findall('c:vertices/c:vertex', NS):
            verts.append((float(v.get('x')), float(v.get('y')), float(v.get('z'))))
        for t in mesh.findall('c:triangles/c:triangle', NS):
            a = base + int(t.get('v1'))
            b = base + int(t.get('v2'))
            c = base + int(t.get('v3'))
            pi = t.get('p1')
            cidx = int(pi) if pi is not None else obj_default
            tris.append((a, b, c, cidx))
    return verts, tris, colors


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else 'openscad/ship.3mf'
    dst = sys.argv[2] if len(sys.argv) > 2 else 'main/objects/ship_model.h'

    verts_scad, tris_raw, colors = load_3mf(src)

    # Axis remap SCAD (x lateral, y forward, z up) -> game (x lateral,
    # y up, z forward): game = (sx, sz, sy).
    verts = [(x, z, y) for (x, y, z) in verts_scad]

    # Centre laterally (x) and front-back (z); base-anchor vertically (y)
    # so the hull's BELLY sits at the origin. game_submit_ship places that
    # belly at SHIP_BASE_Y + ship_y, which is exactly the height the
    # vertical/landing system and the collision box treat as the ship's
    # bottom — so the ship rests cleanly on surfaces and the hitbox lines
    # up with the visible hull. (Centring y instead sinks the lower half
    # of the ship through any surface it lands on.)
    xs = [v[0] for v in verts]
    ys = [v[1] for v in verts]
    zs = [v[2] for v in verts]
    cx = (min(xs) + max(xs)) * 0.5
    cy = min(ys)
    cz = (min(zs) + max(zs)) * 0.5
    verts = [(x - cx, y - cy, z - cz) for (x, y, z) in verts]
    span_x = max(xs) - min(xs)
    scale = TARGET_WINGSPAN / span_x if span_x > 1e-9 else 1.0

    # Tag triangles by region and reverse winding (the axis swap above
    # flips handedness; reversing keeps faces CCW-outward in game space).
    tris = []
    for (a, b, c, cidx) in tris_raw:
        hexcol = colors[cidx] if 0 <= cidx < len(colors) else '#000000'
        region = COLOR_REGION.get(hexcol, DEFAULT_REGION)
        tris.append((a, c, b, region))

    # Roll pivot: the body's vertical centre (model units). The hull is
    # symmetric about its longitudinal axis, so this is the central
    # cylinder's axis — bank should roll about it, not the base-anchored
    # belly (y=0), or the fuselage visibly swings.
    body_ys = [verts[i][1] for (a, b, c, r) in tris
               if r == DEFAULT_REGION for i in (a, b, c)]
    roll_pivot_y = (min(body_ys) + max(body_ys)) * 0.5 if body_ys else 0.0

    # Feature-edge extraction for the wireframe outline.
    def normal(tri):
        a, b, c, _ = tri
        ax, ay, az = verts[a]
        bx, by, bz = verts[b]
        cx_, cy_, cz_ = verts[c]
        ux, uy, uz = bx - ax, by - ay, bz - az
        vx, vy, vz = cx_ - ax, cy_ - ay, cz_ - az
        nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
        ln = math.sqrt(nx * nx + ny * ny + nz * nz) or 1.0
        return (nx / ln, ny / ln, nz / ln)

    normals = [normal(t) for t in tris]
    edge_faces = {}
    for fi, (a, b, c, _) in enumerate(tris):
        for (i, j) in ((a, b), (b, c), (c, a)):
            key = (min(i, j), max(i, j))
            edge_faces.setdefault(key, []).append(fi)

    # Keep an edge only where exactly two faces meet at a GENUINE crease.
    #  * Drop non-manifold / boundary edges (face count != 2): these occur
    #    where the separate solids (body, panel, indicators) touch and
    #    OpenSCAD welds coincident vertices, producing stray junction
    #    lines rather than real silhouette.
    #  * Drop near-parallel normals (coplanar triangulation diagonals).
    #  * Drop near-antiparallel normals (degenerate "knife-fold" edges,
    #    e.g. a thin wing fold) — they otherwise render as lines floating
    #    on a flat surface.
    # i.e. keep only when the crease angle is in [FEATURE_ANGLE_DEG,
    # 180 - FEATURE_ANGLE_DEG].
    cos_lo = math.cos(math.radians(FEATURE_ANGLE_DEG))          # coplanar cutoff
    cos_hi = math.cos(math.radians(180.0 - FEATURE_ANGLE_DEG))  # knife-fold cutoff
    edges = []
    for (i, j), faces in edge_faces.items():
        if len(faces) != 2:
            continue
        # Each part is its own mesh, so an edge's two faces share a region.
        if tris[faces[0]][3] not in OUTLINE_REGIONS:
            continue
        n0, n1 = normals[faces[0]], normals[faces[1]]
        dot = n0[0] * n1[0] + n0[1] * n1[1] + n0[2] * n1[2]
        if cos_hi < dot < cos_lo:
            edges.append((i, j))
    edges.sort()

    assert len(verts) <= 256, "vertex count exceeds uint8 index range"

    # --- emit header ---
    def fmt(v):
        return "%.6ff" % v

    region_counts = {r: 0 for r in REGION_ORDER}
    for (_, _, _, r) in tris:
        region_counts[r] += 1

    out = []
    w = out.append
    w("#pragma once")
    w("")
    w("// AUTO-GENERATED from %s by tools/ship_3mf_to_header.py" % src)
    w("// Do NOT hand-edit the geometry tables below — regenerate from the")
    w("// 3MF instead. You MAY tune the placement macros and the region")
    w("// colours; nothing outside this header needs to change when the")
    w("// model is updated.")
    w("//")
    w("// Coordinates are in game axes (x = lateral, y = up, z = forward),")
    w("// centred laterally + front-back and base-anchored vertically (the")
    w("// hull belly sits at y = 0), in raw model units. Triangles")
    w("// are wound CCW-outward. SHIP_MODEL_SCALE maps model units to world")
    w("// units; the submit code applies scale, offsets, bank and world")
    w("// placement.")
    w("")
    w("#include <stdint.h>")
    w("")
    w("typedef enum {")
    for r in REGION_ORDER:
        w("    %s," % r)
    w("    SHIP_REGION_COUNT")
    w("} ship_region_t;")
    w("")
    w("typedef struct { float x, y, z; } ship_model_vert_t;")
    w("typedef struct { uint8_t a, b, c; uint8_t region; } ship_model_tri_t;")
    w("")
    w("// ---- placement knobs (safe to tweak) ----")
    w("// %.3f = TARGET_WINGSPAN / model lateral extent (%.3f); matches" %
      (scale, span_x))
    w("// 2*SHIP_COLLISION_HALF_W so the hull lines up with its collision box.")
    w("#define SHIP_MODEL_SCALE     %sf" % ("%.6f" % scale))
    w("#define SHIP_MODEL_Y_OFFSET  0.000000f  // world-units vertical nudge")
    w("#define SHIP_MODEL_Z_OFFSET  0.000000f  // world-units forward nudge")
    w("// Bank rolls about this model-y (the hull's central axis), not the")
    w("// base-anchored belly, so the fuselage doesn't swing.")
    w("#define SHIP_MODEL_ROLL_PIVOT_Y %sf" % ("%.6f" % roll_pivot_y))
    w("")
    w("// ---- region colours (ARGB) ----")
    w("// Body is shaded per-face by the submit code; panel + indicators are")
    w("// drawn flat. Indicators default to white ('on'); a later battery")
    w("// module will drive each indicator's colour individually (and skip")
    w("// drawing the panel + indicators entirely when no battery is fitted).")
    w("static const uint32_t SHIP_REGION_COLOR[SHIP_REGION_COUNT] = {")
    w("    [SHIP_REGION_BODY]          = 0xFFD8AA38u,  // gold hull")
    w("    [SHIP_REGION_BATTERY_PANEL] = 0xFF0C0C0Cu,  // near-black panel")
    w("    [SHIP_REGION_INDICATOR_0]   = 0xFFFFFFFFu,  // white ('on')")
    w("    [SHIP_REGION_INDICATOR_1]   = 0xFFFFFFFFu,")
    w("    [SHIP_REGION_INDICATOR_2]   = 0xFFFFFFFFu,")
    w("    [SHIP_REGION_INDICATOR_3]   = 0xFFFFFFFFu,")
    w("};")
    w("#define SHIP_MODEL_OUTLINE_COLOR  0xFF31FBFBu  // cyan ridge")
    w("")
    w("static const ship_model_vert_t SHIP_MODEL_VERTS[] = {")
    for (x, y, z) in verts:
        w("    { %s, %s, %s }," % (fmt(x), fmt(y), fmt(z)))
    w("};")
    w("")
    w("static const ship_model_tri_t SHIP_MODEL_TRIS[] = {")
    for (a, b, c, r) in tris:
        w("    { %3d, %3d, %3d, %s }," % (a, b, c, r))
    w("};")
    w("")
    w("static const uint8_t SHIP_MODEL_EDGES[][2] = {")
    line = "    "
    for (i, j) in edges:
        tok = "{ %3d, %3d }, " % (i, j)
        if len(line) + len(tok) > 78:
            w(line.rstrip())
            line = "    "
        line += tok
    if line.strip():
        w(line.rstrip())
    w("};")
    w("")
    w("#define SHIP_MODEL_VERT_COUNT (sizeof(SHIP_MODEL_VERTS)/sizeof(SHIP_MODEL_VERTS[0]))")
    w("#define SHIP_MODEL_TRI_COUNT  (sizeof(SHIP_MODEL_TRIS)/sizeof(SHIP_MODEL_TRIS[0]))")
    w("#define SHIP_MODEL_EDGE_COUNT (sizeof(SHIP_MODEL_EDGES)/sizeof(SHIP_MODEL_EDGES[0]))")
    w("")

    with open(dst, 'w') as f:
        f.write("\n".join(out))

    sys.stderr.write(
        "wrote %s: %d verts, %d tris, %d edges, scale=%.6f\n" %
        (dst, len(verts), len(tris), len(edges), scale))
    for r in REGION_ORDER:
        sys.stderr.write("  %-26s %3d tris\n" % (r, region_counts[r]))


if __name__ == '__main__':
    main()
