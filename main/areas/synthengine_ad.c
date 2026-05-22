#include "areas/synthengine_ad.h"

#include <math.h>

#include "magicnumbers.h"           // GAME_TRI_HALF_W
#include "objects/booster.h"
#include "objects/checkpoint.h"
#include "objects/jump_booster.h"
#include "objects/shield.h"
#include "objects/synthengine_sign.h"
#include "objects/tri.h"
#include "objects/wall.h"           // WALL_SEGMENT_LEN, WALL_SEGMENT_HALF_D
#include "world.h"

// Round-robin ad texts. EASY TO EDIT: add or remove entries here and the
// rest follows automatically. The cursor advances per spawned sign and
// resets to the first entry at the start of each run (synthengine_ad_reset).
static char const* const AD_TEXTS[] = {
    "Race The Synth",
    "SynthEngine 3D",
    "(C) 2026 cavac",
    "Your AD here?",
    "[CENSORED]",
    "16 16 16",
    "Vote El Presidente",
};
#define AD_TEXT_COUNT ((int)(sizeof(AD_TEXTS) / sizeof(AD_TEXTS[0])))

static int s_ad_next = 0;   // index of the next ad text to display

// Ad text colour (ARGB). Single colour for this area; the sign object
// takes it per-spawn, so other sign uses can pick their own. (A parallel
// per-text colour array would slot in here trivially if ads ever want
// individual colours.)
#define AD_TEXT_COLOR 0xFFFF0000u   // red

void synthengine_ad_reset(void) { s_ad_next = 0; }

// Three wall-segments deep — a very short area.
#define AD_AREA_DEPTH (3.0f * WALL_SEGMENT_LEN)

// Number of distinct pickup kinds that can appear under the sign.
#define AD_PICKUP_KINDS 5

// Snap a spawn z down to the nearest wall-segment centre at or below
// `target`, so the sign sits cleanly on the wall grid (same as bridges).
static float snap_to_wall_segment(float target) {
    int const n = (int)floorf((target - WALL_SEGMENT_HALF_D) / WALL_SEGMENT_LEN);
    return (float)n * WALL_SEGMENT_LEN + WALL_SEGMENT_HALF_D;
}

void area_synthengine_ad_init(area_state_t* a, uint16_t stage, uint32_t* prng) {
    (void)stage;   // stage-agnostic — can occur in any level
    a->type               = AREA_TYPE_SYNTHENGINE_AD;
    a->length_remaining_z = AD_AREA_DEPTH;
    a->next_event_z       = 0.0f;   // spawn on the first tick
    a->gates_remaining    = 1;      // exactly one sign

    // Pick the under-sign pickup kind (0..AD_PICKUP_KINDS-1) and a random
    // lateral position for it, stored in the overloaded area slots
    // (passage_mirror = kind, gate_hole_x = x — see world.h).
    int kind = (int)(world_frand(prng) * (float)AD_PICKUP_KINDS);
    if (kind >= AD_PICKUP_KINDS) kind = AD_PICKUP_KINDS - 1;
    a->passage_mirror = kind;

    float const inset = TRACK_HALF_WIDTH - GAME_TRI_HALF_W;
    a->gate_hole_x = (world_frand(prng) * 2.0f - 1.0f) * inset;
}

// Spawn one pickup of the chosen kind at (x, z).
static void spawn_under_sign(world_state_t* w, int kind, float x, float z) {
    switch (kind) {
        case 0:  tri_spawn_at(w, x, z);          break;
        case 1:  booster_spawn_at(w, x, z);      break;
        case 2:  jump_booster_spawn_at(w, x, z); break;
        case 3:  shield_spawn_at(w, x, z);       break;
        case 4:  checkpoint_spawn_at(w, x, z);   break;
        default: tri_spawn_at(w, x, z);          break;
    }
}

bool area_synthengine_ad_tick(world_state_t* w, area_state_t* a, float dz) {
    a->length_remaining_z -= dz;
    a->next_event_z       -= dz;

    if (a->gates_remaining > 0 && a->next_event_z <= 0.0f) {
        float const z = snap_to_wall_segment(WORLD_Z_FAR_SPAWN);
        synthengine_sign_spawn(w, z, AD_TEXTS[s_ad_next], AD_TEXT_COLOR);
        s_ad_next = (s_ad_next + 1) % AD_TEXT_COUNT;
        spawn_under_sign(w, a->passage_mirror, a->gate_hole_x, z);
        a->gates_remaining = 0;
    }
    return a->length_remaining_z <= 0.0f;
}
