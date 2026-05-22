#!/usr/bin/env python3
# Convert a multi-part OpenSCAD 3MF into a C header for a world object.
#
# Generalises tools/ship_3mf_to_header.py for ordinary world objects that
# are placed by an obstacle emit callback (not the ship). Differences from
# the ship tool: region-TAGGED triangles AND edges (so the emit code can
# colour the outline per region), per-region fill + outline colour tables,
# and a configurable vertical anchor (centre vs base) and scale anchor
# (lateral wingspan vs total height). The ship keeps its own tool; this is
# the path for new objects. (The two could be merged later.)
#
# Pipeline mirrors the ship tool: parse 3MF (each part is its own
# <object>/<mesh> with mesh-LOCAL indices -> concatenate with offsets),
# remap SCAD axes (x lateral, y forward, z up) to game axes (x lateral,
# y up, z forward) = (sx, sz, sy), reverse winding (the swap flips
# handedness) so faces stay CCW-outward, place the origin per config,
# extract crease/feature edges, emit the header.
#
# Usage: tools/object_3mf_to_header.py <object_key>
#   e.g. tools/object_3mf_to_header.py restmark

import math
import sys
import xml.etree.ElementTree as ET
import zipfile

NS = {
    'c': 'http://schemas.microsoft.com/3dmanufacturing/core/2015/02',
    'm': 'http://schemas.microsoft.com/3dmanufacturing/material/2015/02',
}

FEATURE_ANGLE_DEG = 18.0  # crease band is [FEATURE, 180-FEATURE]

# Per-object configuration registry.
OBJECTS = {
    'restmark': {
        'src': 'openscad/restarea_markers.3mf',
        'dst': 'main/objects/restarea_marker_model.h',
        'prefix': 'RESTMARK',                 # symbol / macro prefix
        'enum': 'restmark_region_t',
        'vert_type': 'restmark_vert_t',
        'tri_type': 'restmark_tri_t',
        'edge_type': 'restmark_edge_t',
        # OpenSCAD colour (#RRGGBB) -> region; anything else -> default.
        'color_region': {'#00FF00': 'RESTMARK_REGION_BEACON'},
        'default_region': 'RESTMARK_REGION_POST',
        'region_order': ['RESTMARK_REGION_POST', 'RESTMARK_REGION_BEACON'],
        # Default per-region fill (ARGB). The beacon's fill is overridden
        # at runtime by the pulse; this is just the bright end.
        'region_fill': {
            'RESTMARK_REGION_POST':   '0xFFA0A0A0u',  # grey stalk
            'RESTMARK_REGION_BEACON': '0xFF00FF00u',  # full green
        },
        # Per-region wireframe colour.
        'region_outline': {
            'RESTMARK_REGION_POST':   '0xFFFFFFFFu',  # white
            'RESTMARK_REGION_BEACON': '0xFF404040u',  # neutral dark grey
        },
        'outline_regions': {'RESTMARK_REGION_POST', 'RESTMARK_REGION_BEACON'},
        'scale': ('height', 2.0),   # total world-units height standing up
        'vanchor': 'base',          # min-y sits at the origin (on the wall)
    },
}


def load_3mf(path):
    with zipfile.ZipFile(path) as z:
        with z.open('3D/3dmodel.model') as f:
            root = ET.parse(f).getroot()
    colors = []
    cg = root.find('.//m:colorgroup', NS)
    if cg is not None:
        for col in cg.findall('m:color', NS):
            colors.append(col.get('color')[:7].upper())
    # Each part is its own <object>/<mesh> with mesh-local indices; merge
    # with a running vertex offset (no build-item transforms present).
    verts, tris = [], []
    for obj in root.findall('.//c:object', NS):
        mesh = obj.find('c:mesh', NS)
        if mesh is None:
            continue
        obj_default = int(obj.get('pindex') or 0)
        base = len(verts)
        for v in mesh.findall('c:vertices/c:vertex', NS):
            verts.append((float(v.get('x')), float(v.get('y')), float(v.get('z'))))
        for t in mesh.findall('c:triangles/c:triangle', NS):
            pi = t.get('p1')
            cidx = int(pi) if pi is not None else obj_default
            tris.append((base + int(t.get('v1')),
                         base + int(t.get('v2')),
                         base + int(t.get('v3')), cidx))
    return verts, tris, colors


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in OBJECTS:
        sys.exit("usage: object_3mf_to_header.py <%s>" % '|'.join(OBJECTS))
    cfg = OBJECTS[sys.argv[1]]
    src, dst, P = cfg['src'], cfg['dst'], cfg['prefix']

    verts_scad, tris_raw, colors = load_3mf(src)

    # SCAD (x lateral, y forward, z up) -> game (x lateral, y up, z fwd).
    verts = [(x, z, y) for (x, y, z) in verts_scad]
    xs = [v[0] for v in verts]
    ys = [v[1] for v in verts]
    zs = [v[2] for v in verts]
    cx = (min(xs) + max(xs)) * 0.5
    cz = (min(zs) + max(zs)) * 0.5
    # Vertical: centre, or anchor the base (min y) at the origin so the
    # object stands up from where it is placed (e.g. on a wall top).
    cy = (min(ys) + max(ys)) * 0.5 if cfg['vanchor'] == 'center' else min(ys)
    verts = [(x - cx, y - cy, z - cz) for (x, y, z) in verts]

    mode, target = cfg['scale']
    if mode == 'height':
        extent = max(ys) - min(ys)
    elif mode == 'wingspan':
        extent = max(xs) - min(xs)
    else:
        extent = 1.0
    scale = target / extent if extent > 1e-9 else 1.0

    # Tag triangles by region; reverse winding (axis swap flipped it).
    tris = []
    for (a, b, c, cidx) in tris_raw:
        hexcol = colors[cidx] if 0 <= cidx < len(colors) else '#000000'
        region = cfg['color_region'].get(hexcol, cfg['default_region'])
        tris.append((a, c, b, region))

    def normal(t):
        a, b, c, _ = t
        ax, ay, az = verts[a]; bx, by, bz = verts[b]; cx_, cy_, cz_ = verts[c]
        ux, uy, uz = bx - ax, by - ay, bz - az
        vx, vy, vz = cx_ - ax, cy_ - ay, cz_ - az
        nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
        ln = math.sqrt(nx * nx + ny * ny + nz * nz) or 1.0
        return (nx / ln, ny / ln, nz / ln)

    normals = [normal(t) for t in tris]
    edge_faces = {}
    for fi, (a, b, c, _) in enumerate(tris):
        for (i, j) in ((a, b), (b, c), (c, a)):
            edge_faces.setdefault((min(i, j), max(i, j)), []).append(fi)

    cos_lo = math.cos(math.radians(FEATURE_ANGLE_DEG))
    cos_hi = math.cos(math.radians(180.0 - FEATURE_ANGLE_DEG))
    edges = []  # (i, j, region)
    for (i, j), faces in edge_faces.items():
        if len(faces) != 2:
            continue
        region = tris[faces[0]][3]
        if region not in cfg['outline_regions']:
            continue
        n0, n1 = normals[faces[0]], normals[faces[1]]
        dot = n0[0] * n1[0] + n0[1] * n1[1] + n0[2] * n1[2]
        if cos_hi < dot < cos_lo:
            edges.append((i, j, region))
    edges.sort()

    assert len(verts) <= 256, "vertex count exceeds uint8 index range"

    out = []
    w = out.append
    w("#pragma once")
    w("")
    w("// AUTO-GENERATED from %s by tools/object_3mf_to_header.py" % src)
    w("// Do NOT hand-edit the geometry tables — regenerate from the 3MF.")
    w("// Coordinates are game axes (x lateral, y up, z forward), %s-anchored"
      % cfg['vanchor'])
    w("// vertically and centred laterally, in raw model units; triangles are")
    w("// CCW-outward. %s_SCALE maps model units to world units." % P)
    w("")
    w("#include <stdint.h>")
    w("")
    w("typedef enum {")
    for r in cfg['region_order']:
        w("    %s," % r)
    w("    %s_REGION_COUNT" % P)
    w("} %s;" % cfg['enum'])
    w("")
    w("typedef struct { float x, y, z; } %s;" % cfg['vert_type'])
    w("typedef struct { uint8_t a, b, c, region; } %s;" % cfg['tri_type'])
    w("typedef struct { uint8_t a, b, region; } %s;" % cfg['edge_type'])
    w("")
    w("// %.3f = target %s (%.3f) / model extent" % (scale, mode, target))
    w("#define %s_SCALE     %sf" % (P, "%.6f" % scale))
    w("#define %s_Y_OFFSET  0.000000f  // world-units vertical nudge" % P)
    w("#define %s_Z_OFFSET  0.000000f  // world-units forward nudge" % P)
    w("")
    w("// Per-region fill colour (ARGB). The emit code may override these")
    w("// (e.g. a pulsing beacon).")
    w("static const uint32_t %s_REGION_FILL[%s_REGION_COUNT] = {" % (P, P))
    for r in cfg['region_order']:
        w("    [%s] = %s," % (r, cfg['region_fill'][r]))
    w("};")
    w("// Per-region wireframe colour (ARGB).")
    w("static const uint32_t %s_REGION_OUTLINE[%s_REGION_COUNT] = {" % (P, P))
    for r in cfg['region_order']:
        w("    [%s] = %s," % (r, cfg['region_outline'][r]))
    w("};")
    w("")
    w("static const %s %s_VERTS[] = {" % (cfg['vert_type'], P))
    for (x, y, z) in verts:
        w("    { %.6ff, %.6ff, %.6ff }," % (x, y, z))
    w("};")
    w("")
    w("static const %s %s_TRIS[] = {" % (cfg['tri_type'], P))
    for (a, b, c, r) in tris:
        w("    { %3d, %3d, %3d, %s }," % (a, b, c, r))
    w("};")
    w("")
    w("static const %s %s_EDGES[] = {" % (cfg['edge_type'], P))
    for (i, j, r) in edges:
        w("    { %3d, %3d, %s }," % (i, j, r))
    w("};")
    w("")
    w("#define %s_VERT_COUNT (sizeof(%s_VERTS)/sizeof(%s_VERTS[0]))" % (P, P, P))
    w("#define %s_TRI_COUNT  (sizeof(%s_TRIS)/sizeof(%s_TRIS[0]))" % (P, P, P))
    w("#define %s_EDGE_COUNT (sizeof(%s_EDGES)/sizeof(%s_EDGES[0]))" % (P, P, P))
    w("")

    with open(dst, 'w') as f:
        f.write("\n".join(out))

    counts = {r: 0 for r in cfg['region_order']}
    for (_, _, _, r) in tris:
        counts[r] += 1
    sys.stderr.write("wrote %s: %d verts, %d tris, %d edges, scale=%.6f\n" %
                     (dst, len(verts), len(tris), len(edges), scale))
    for r in cfg['region_order']:
        sys.stderr.write("  %-26s %3d tris\n" % (r, counts[r]))


if __name__ == '__main__':
    main()
