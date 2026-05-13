#pragma once

#include <stdbool.h>
#include <stdint.h>

struct area_state_s;
struct world_state_s;

void area_big_blocks_init(struct area_state_s* a, uint16_t stage, uint32_t* prng);
bool area_big_blocks_tick(struct world_state_s* w, struct area_state_s* a, float dz);
