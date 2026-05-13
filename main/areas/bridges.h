#pragma once

#include <stdbool.h>
#include <stdint.h>

struct area_state_s;
struct world_state_s;

// 1..5 concrete bridges spanning the playfield. Stage-agnostic
// (difficulty doesn't scale). Each bridge occupies one wall-segment
// of z; gaps between bridges are the same depth. The booster-owed
// counter is honoured (booster lands at random x on the floor) so
// the scheduler can still drop a boost during a bridges run.
void area_bridges_init(struct area_state_s* a, uint16_t stage, uint32_t* prng);

bool area_bridges_tick(struct world_state_s* w, struct area_state_s* a, float dz);
