#pragma once

#include <stdbool.h>
#include <stdint.h>

struct area_state_s;
struct world_state_s;

// Set `*a` to a freshly-initialised pixel-field area for `stage`.
// Reads from `*prng` so length budget + first event interval are
// deterministic from the stage seed.
void area_pixel_field_init(struct area_state_s* a, uint16_t stage, uint32_t* prng);

// Tick the area by `dz` world-z units. Spawns due events into the
// shared obstacle pool (consuming boosters_owed when one is due).
// Returns true if the area's length budget is exhausted.
bool area_pixel_field_tick(struct world_state_s* w, struct area_state_s* a, float dz);
