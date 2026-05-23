#include "objects/synthengine_sign.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "game.h"           // SHIP_* collision constants, game_state_s
#include "objects/synthengine_sign_model.h"
#include "objects/wall.h"   // WALL_HEIGHT
#include "render.h"         // render_camera, RENDER_NEAR_CLIP_Z
#include "scene.h"          // scene_tri, scene_line
#include "se_text.h"        // simplex[] Hershey glyph table (public engine API)
#include "world.h"          // obstacle_spawn

// We draw the sign text as raw Hershey strokes mapped onto 3D geometry,
// so we read the engine's `simplex[]` glyph table directly (declared in
// se_text.h). Format per glyph: [nverts, advance, x0,y0, x1,y1, ...],
// with (-1,-1) marking a pen-up. Y is already up (0 = baseline,
// 21 = cap height), so it maps straight into world-up with no flip.
#define HERSHEY_CAP_H 21.0f

// --- Panel text layout (world units; model is 1:1 so scale ~ 1) -------
// Usable text width across the panel (panel face is x in ~[-4.5, 4.5]);
// long strings auto-shrink to fit this. Max cap height clamps short
// strings so they don't tower over the panel (panel is y 7..10, 3 tall).
#define SIGN_TEXT_WIDTH   8.0f
#define SIGN_TEXT_MAX_H   2.0f
// Text colour is per-sign — set by the caller at spawn (stored in scratch),
// so the same object renders different signage in different colours.
// Panel spans model-y [7, 10]; centre at 8.5. Text sits just in front of
// the panel's near (camera-facing) face so it isn't z-fought or hidden.
#define SIGN_PANEL_MID_Y  8.5f
#define SIGN_PANEL_NEAR_Z 0.125f        // panel half-depth (model units)
#define SIGN_TEXT_Z_BIAS  0.03f         // nudge toward camera

// Collidable region (model units, scaled at runtime): the sign panel +
// corner holders, x in [-5, 5], y in [7, 10] above the base. The pillars
// (x = +/-5.5, on the walls) and the text are deliberately NOT collidable.
#define SIGN_PANEL_HALF_W 5.0f
#define SIGN_PANEL_LO_Y   7.0f
#define SIGN_PANEL_HI_Y   10.0f

// Private per-obstacle state overlaid on the scratch buffer: the text to
// draw (copied at spawn so the caller's string need not persist).
typedef struct {
    char     text[48];
    uint32_t text_color;   // ARGB, set at spawn
} sign_scratch_t;

// Scale each RGB channel of an ARGB colour by `f` (0..1); alpha kept.
static inline pax_col_t sign_dim(pax_col_t c, float f) {
    uint32_t const a = (c >> 24) & 0xFF;
    uint32_t const r = (uint32_t)((float)((c >> 16) & 0xFF) * f);
    uint32_t const g = (uint32_t)((float)((c >>  8) & 0xFF) * f);
    uint32_t const b = (uint32_t)((float)((c >>  0) & 0xFF) * f);
    return (pax_col_t)((a << 24) | (r << 16) | (g << 8) | b);
}

// Total horizontal advance of `text` in Hershey font units.
static float text_advance_units(char const* text) {
    float u = 0.0f;
    for (char const* p = text; *p; p++) {
        int const idx = (int)(unsigned char)*p - 32;
        u += (idx >= 0 && idx < 95) ? (float)simplex[idx][1] : 16.0f;
    }
    return u;
}

// Emit the string as world-space line segments (the panel face is a
// constant-z plane; x is lateral, y is up). `left_x` is the pen start,
// `baseline_y` the glyph baseline, `scale` font-units -> world units.
static void emit_text_strokes(char const* text, float left_x, float baseline_y,
                              float z, float scale, uint32_t color) {
    float pen_x = left_x;
    for (char const* p = text; *p; p++) {
        int const idx = (int)(unsigned char)*p - 32;
        if (idx < 0 || idx >= 95) { pen_x += 16.0f * scale; continue; }
        int const* glyph   = simplex[idx];
        int const  nverts  = glyph[0];
        int const  advance = glyph[1];
        int   pen_down = 0;
        float px = 0.0f, py = 0.0f;
        for (int i = 0; i < nverts; i++) {
            int const vx = glyph[2 + i * 2];
            int const vy = glyph[2 + i * 2 + 1];
            if (vx == -1 && vy == -1) { pen_down = 0; continue; }
            float const wx = pen_x + (float)vx * scale;
            float const wy = baseline_y + (float)vy * scale;   // y up, no flip
            if (pen_down) scene_line(px, py, z, wx, wy, z, color);
            px = wx; py = wy; pen_down = 1;
        }
        pen_x += (float)advance * scale;
    }
}

static void synthengine_sign_emit(obstacle_t const* o) {
    if (o->z_world < RENDER_NEAR_CLIP_Z) return;

    float const s = SYNTHSIGN_SCALE;
    // The model is base-anchored: local y = 0 sits on the wall tops. We
    // anchor the *visual* at WALL_HEIGHT directly, NOT at o->y_base —
    // o->y_base is the collision/shadow box (the elevated panel), a
    // different height. Keeping the visual on WALL_HEIGHT lets the box be
    // just the panel while the emit still draws the whole gantry.
    float const base_y = WALL_HEIGHT;
    float wx[SYNTHSIGN_VERT_COUNT], wy[SYNTHSIGN_VERT_COUNT], wz[SYNTHSIGN_VERT_COUNT];
    for (size_t i = 0; i < SYNTHSIGN_VERT_COUNT; i++) {
        synthsign_vert_t const* v = &SYNTHSIGN_VERTS[i];
        wx[i] = o->x_world + v->x * s;
        wy[i] = base_y     + v->y * s + SYNTHSIGN_Y_OFFSET;
        wz[i] = o->z_world + v->z * s + SYNTHSIGN_Z_OFFSET;
    }

    // Fixed world-space light (front-top-left), matching the other
    // hard-surface objects.
    float const light_x = -0.4f, light_y = 0.7f, light_z = -0.6f;
    render_camera_t const cam = render_camera();

    for (size_t i = 0; i < SYNTHSIGN_TRI_COUNT; i++) {
        synthsign_tri_t const* t = &SYNTHSIGN_TRIS[i];
        int const a = t->a, b = t->b, c = t->c;

        float const ux = wx[b] - wx[a], uy = wy[b] - wy[a], uz = wz[b] - wz[a];
        float const vx = wx[c] - wx[a], vy = wy[c] - wy[a], vz = wz[c] - wz[a];
        float const nx = uy * vz - uz * vy;
        float const ny = uz * vx - ux * vz;
        float const nz = ux * vy - uy * vx;

        // Back-face cull against the camera at (cam.x, cam.y, 0).
        float const fcx = (wx[a] + wx[b] + wx[c]) * (1.0f / 3.0f);
        float const fcy = (wy[a] + wy[b] + wy[c]) * (1.0f / 3.0f);
        float const fcz = (wz[a] + wz[b] + wz[c]) * (1.0f / 3.0f);
        if (nx * (cam.x - fcx) + ny * (cam.y - fcy) + nz * (0.0f - fcz) <= 0.0f) {
            continue;
        }

        // Per-face directional lighting so the gantry reads as 3D.
        float const nlen = sqrtf(nx * nx + ny * ny + nz * nz);
        float const inv  = (nlen > 1e-6f) ? (1.0f / nlen) : 0.0f;
        float       d    = (nx * inv) * light_x + (ny * inv) * light_y + (nz * inv) * light_z;
        if (d < 0.0f) d = 0.0f;
        pax_col_t const col = sign_dim((pax_col_t)SYNTHSIGN_REGION_FILL[t->region],
                                       0.55f + 0.45f * d);
        scene_tri(wx[a], wy[a], wz[a], wx[b], wy[b], wz[b], wx[c], wy[c], wz[c], col);
    }

    for (size_t i = 0; i < SYNTHSIGN_EDGE_COUNT; i++) {
        synthsign_edge_t const* e = &SYNTHSIGN_EDGES[i];
        scene_line(wx[e->a], wy[e->a], wz[e->a],
                   wx[e->b], wy[e->b], wz[e->b],
                   (pax_col_t)SYNTHSIGN_REGION_OUTLINE[e->region]);
    }

    // The ad text — Hershey strokes on the panel's camera-facing face,
    // auto-fitted to the panel width and centred.
    sign_scratch_t const* sc = (sign_scratch_t const*)o->scratch;
    if (sc->text[0] != '\0') {
        float const units = text_advance_units(sc->text);
        float       scale = SIGN_TEXT_MAX_H / HERSHEY_CAP_H;
        if (units > 0.0f) {
            float const fit = SIGN_TEXT_WIDTH / units;
            if (fit < scale) scale = fit;
        }
        float const total_w    = units * scale;
        float const left_x     = o->x_world - total_w * 0.5f;
        float const baseline_y = base_y + SIGN_PANEL_MID_Y * s - (HERSHEY_CAP_H * scale) * 0.5f;
        float const z          = o->z_world - SIGN_PANEL_NEAR_Z * s - SIGN_TEXT_Z_BIAS;
        emit_text_strokes(sc->text, left_x, baseline_y, z, scale, sc->text_color);
    }
}

obstacle_t* synthengine_sign_spawn(world_state_t* w, float z,
                                   char const* text, uint32_t text_color) {
    // The obstacle's collision/shadow box IS the sign panel + holders —
    // an elevated slab up at y ~ 7.7-10.7 (model y 7-10 above the wall
    // top). Default cube dispatch then handles both: a head-on crash if
    // the ship ever reaches it, and (via the now elevation-aware
    // render_shadows + game.c shadow ray) its floor shadow. The pillars
    // and the open gap below are NOT in the box, so flying under is
    // free and the pillars cast no shadow. The full gantry + the ad text
    // are drawn by the emit, which anchors the visual at WALL_HEIGHT
    // independently of this box.
    float const s = SYNTHSIGN_SCALE;
    obstacle_t* const o = obstacle_spawn(
        w, OBSTACLE_KIND_CUBE,
        0.0f, z,
        SIGN_PANEL_HALF_W * s, 0.125f * s, (SIGN_PANEL_HI_Y - SIGN_PANEL_LO_Y) * s,
        SYNTHSIGN_REGION_FILL[SYNTHSIGN_REGION_PANEL],
        SYNTHSIGN_REGION_FILL[SYNTHSIGN_REGION_PILLAR],
        SYNTHSIGN_REGION_FILL[SYNTHSIGN_REGION_PANEL],
        SYNTHSIGN_REGION_OUTLINE[SYNTHSIGN_REGION_PANEL]);
    if (o) {
        o->y_base = WALL_HEIGHT + SIGN_PANEL_LO_Y * s;   // elevated panel slab
        o->emit   = synthengine_sign_emit;               // draws the full gantry + text
        sign_scratch_t* sc = (sign_scratch_t*)o->scratch;
        snprintf(sc->text, sizeof(sc->text), "%s", text ? text : "");
        sc->text_color = text_color;
    }
    return o;
}
