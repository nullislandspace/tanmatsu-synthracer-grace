#pragma once

#include <stdbool.h>
#include <stdint.h>

struct area_state_s;
struct world_state_s;

// "SynthEngine Ad" — a very short area (three wall-segments deep) that
// spawns a single gantry sign spanning the track (objects/synthengine_sign),
// plus one randomly-chosen pickup (Tri / booster / jump / shield /
// checkpoint) somewhere under it. Stage-agnostic; can occur in any level.
// The sign's text cycles round-robin through a fixed list (see the .c) so
// successive ads differ; the cursor resets to the first entry each run.
void area_synthengine_ad_init(struct area_state_s* a, uint16_t stage, uint32_t* prng);

bool area_synthengine_ad_tick(struct world_state_s* w, struct area_state_s* a, float dz);

// Reset the round-robin ad-text cursor to the first entry. Call once per
// run (from world_init) so every run opens on the same ad.
void synthengine_ad_reset(void);
