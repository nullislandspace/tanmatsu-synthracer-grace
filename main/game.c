#include "game.h"

#include <math.h>

#include "pax_gfx.h"
#include "render.h"
#include "shapes/pax_tris.h"

// --- Lateral motion -----------------------------------------------------------

#define SHIP_X_MIN_WORLD     -5.0f
#define SHIP_X_MAX_WORLD      5.0f

// Visual maximum bank in radians (~31°). This is multiplied by the
// signed bank factor in game_state_t, which itself is clamped to
// [-1, +1]. Kept moderate so the wing tips don't clip into the
// ground at SHIP_BASE_Y.
#define MAX_BANK_RAD          0.55f

// How many bank-units per second the ship rolls toward the steer
// target. ACTIVE applies whenever the player is holding any steer
// direction — including the opposite of the current bank — so
// reversing the stick snaps the ship over fast. PASSIVE is the
// slower self-righting rate that takes over when the stick is
// released; the asymmetry is what makes "let go and drift back"
// feel different from "yank the stick the other way".
#define BANK_ACTIVE_RATE      3.5f
#define BANK_PASSIVE_RATE     1.0f

// Lateral world-units travelled per second at full bank
// (|bank|=1.0). Lateral velocity scales linearly with bank, so the
// longer the player holds a direction the sharper the turn — bank
// ramps up over time, and the turn ramps with it. SHIP_X_MAX_WORLD
// clamps the position; lateral velocity is otherwise unbounded by
// itself.
#define SHIP_TURN_RATE        3.5f

// --- Visuals ------------------------------------------------------------------

// Tetrahedron mesh: the nose is the elevated apex, both wing tips
// and the tail sit on the ship's ventral plane. From the camera's
// chase angle (above-behind), the silhouette is a 4-point diamond
// with no interior vertices, so the two visible roof panels split
// the screen along the nose-tail centerline and never overlap each
// other.
//
// Per-face palette: the two roof panels get slightly different
// yellows — read as a sharp colour break along the central ridge,
// which is the 3D cue the previous flattened-diamond design was
// missing. The two belly faces are magenta and only become
// visible when banking exposes the underside.
#define SHIP_ROOF_LEFT_COLOR   0xFFFFFF6Bu  // sun-yellow
#define SHIP_ROOF_RIGHT_COLOR  0xFFD8AA38u  // dimmer yellow
#define SHIP_BELLY_COLOR       0xFFF71FF1u  // grid-magenta
#define SHIP_RIDGE_COLOR       0xFF31FBFBu  // cyan accent

typedef struct {
    float x, y, z;
} ship_vert_t;

static ship_vert_t const ship_verts[] = {
    [0] = {  0.00f,  0.30f,   0.32f},  // nose (elevated apex)
    [1] = { -0.28f,  0.00f,  -0.10f},  // left  wing tip (ground level)
    [2] = {  0.28f,  0.00f,  -0.10f},  // right wing tip (ground level)
    [3] = {  0.00f,  0.00f,  -0.36f},  // tail (ground level)
};

typedef enum {
    SHIP_FACE_BELLY = 0,
    SHIP_FACE_ROOF_LEFT,
    SHIP_FACE_ROOF_RIGHT,
} ship_face_t;

typedef struct {
    uint8_t     a, b, c;
    ship_face_t face;
} ship_tri_t;

static ship_tri_t const ship_tris[] = {
    // Underside — front belly (nose to wing line) and back belly
    // (wing line to tail). Drawn first; for normal upright flight
    // the roof tris paint over them.
    {0, 2, 1, SHIP_FACE_BELLY},  // nose / Rwing / Lwing
    {1, 2, 3, SHIP_FACE_BELLY},  // Lwing / Rwing / tail
    // Roof panels: each is a single triangle spanning nose → wing
    // → tail along one side. They share the nose-tail ridge.
    {0, 1, 3, SHIP_FACE_ROOF_LEFT},   // nose / Lwing / tail
    {0, 3, 2, SHIP_FACE_ROOF_RIGHT},  // nose / tail / Rwing
};

// Cyan accent lines: silhouette outline (nose → Lwing → tail →
// Rwing → nose) plus the dorsal ridge (nose → tail). Drawing the
// silhouette as well as the ridge reads as a clearer wire-frame
// hull from the chase view.
static uint8_t const ship_outline_edges[][2] = {
    {0, 1},  // nose → Lwing
    {1, 3},  // Lwing → tail
    {3, 2},  // tail → Rwing
    {2, 0},  // Rwing → nose
    {0, 3},  // nose → tail (ridge)
};

#define SHIP_VERT_COUNT    (sizeof(ship_verts) / sizeof(ship_verts[0]))
#define SHIP_TRI_COUNT     (sizeof(ship_tris)  / sizeof(ship_tris[0]))
#define SHIP_OUTLINE_COUNT (sizeof(ship_outline_edges) / sizeof(ship_outline_edges[0]))

// --- Public API ---------------------------------------------------------------

void game_init(game_state_t* g) {
    g->ship_x_world = 0.0f;
    g->ship_speed_z = SHIP_BASE_SPEED_Z;
    g->bank         = 0.0f;
    g->cam_x        = 0.0f;
}

void game_step(game_state_t* g, float dt, int steer) {
    if (dt <= 0.0f) return;

    // Bank target is the steer direction itself: full bank when
    // holding, zero target when released. The rate flips between
    // the active and passive rates depending on whether any steer
    // is being held — an opposite hold uses the active rate for
    // the whole flip, which is the asymmetry the player feels.
    float const target   = (float)steer;
    float const rate     = (steer != 0) ? BANK_ACTIVE_RATE : BANK_PASSIVE_RATE;
    float const max_step = rate * dt;
    float       delta    = target - g->bank;
    if (delta >  max_step) delta =  max_step;
    if (delta < -max_step) delta = -max_step;
    g->bank += delta;

    // Lateral velocity is purely a function of bank. Smooth ramps in
    // bank produce smooth lateral motion without needing a separate
    // friction model.
    g->ship_x_world += g->bank * SHIP_TURN_RATE * dt;
    if (g->ship_x_world > SHIP_X_MAX_WORLD) {
        g->ship_x_world = SHIP_X_MAX_WORLD;
    } else if (g->ship_x_world < SHIP_X_MIN_WORLD) {
        g->ship_x_world = SHIP_X_MIN_WORLD;
    }

    // Camera locked to the ship laterally — the ship stays centred on
    // screen and the world pans around it.
    g->cam_x = g->ship_x_world;
}

void game_draw_ship(pax_buf_t* fb, game_state_t const* g) {
    // Roll about the forward (z) axis. Positive bank → right wing
    // dips. The 2D rotation in the (x, y) plane is:
    //     x' =  x cos θ + y sin θ
    //     y' = -x sin θ + y cos θ
    // with θ = bank * MAX_BANK_RAD.
    float const angle = g->bank * MAX_BANK_RAD;
    float const c     = cosf(angle);
    float const s     = sinf(angle);

    pax_vec2f screen[SHIP_VERT_COUNT];
    for (size_t i = 0; i < SHIP_VERT_COUNT; i++) {
        ship_vert_t const* v = &ship_verts[i];
        float const lx = v->x * c + v->y * s;
        float const ly = -v->x * s + v->y * c;
        float const wx = lx + g->ship_x_world;
        float const wy = ly + SHIP_BASE_Y;
        float const wz = v->z + SHIP_Z_PLANE;
        render_project(wx, wy, wz, g->cam_x, &screen[i].x, &screen[i].y);
    }

    for (size_t i = 0; i < SHIP_TRI_COUNT; i++) {
        ship_tri_t const* t   = &ship_tris[i];
        pax_col_t         col = SHIP_BELLY_COLOR;
        switch (t->face) {
            case SHIP_FACE_ROOF_LEFT:  col = SHIP_ROOF_LEFT_COLOR;  break;
            case SHIP_FACE_ROOF_RIGHT: col = SHIP_ROOF_RIGHT_COLOR; break;
            case SHIP_FACE_BELLY:      col = SHIP_BELLY_COLOR;      break;
        }
        pax_simple_tri(fb, col,
                       screen[t->a].x, screen[t->a].y,
                       screen[t->b].x, screen[t->b].y,
                       screen[t->c].x, screen[t->c].y);
    }

    // Cockpit ridges as cyan accent lines on top of the fills.
    for (size_t i = 0; i < SHIP_OUTLINE_COUNT; i++) {
        uint8_t const a = ship_outline_edges[i][0];
        uint8_t const b = ship_outline_edges[i][1];
        pax_simple_line(fb, SHIP_RIDGE_COLOR,
                        screen[a].x, screen[a].y,
                        screen[b].x, screen[b].y);
    }
}
