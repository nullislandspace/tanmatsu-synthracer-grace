#pragma once

#include <stdbool.h>
#include <stdint.h>

struct area_state_s;
struct world_state_s;

void area_gateways_init(struct area_state_s* a, uint8_t stage, uint32_t* prng);
bool area_gateways_tick(struct world_state_s* w, struct area_state_s* a, float dz);
