#pragma once

#include <stdbool.h>
#include <stdint.h>

struct area_state_s;
struct world_state_s;

// Dynamic gateway — a sequence of gateway walls (like the standard
// gateways area) with two distinguishing rules:
//   1. The hole's x is picked once at area-init and shared by every
//      wall in this area run. The player learns the hole position
//      from the first wall and can pre-align for the rest.
//   2. Right behind each hole sits a flipping cube whose width
//      exactly equals the hole. The cube blocks the hole until it
//      rolls away as the player approaches — roll direction is
//      always toward the centre (hole right of centre → cube rolls
//      LEFT, hole left of centre → cube rolls RIGHT). Hole-at-
//      centre snaps to left-roll, so there's no deadband.
//
// Hole-x is re-drawn from the stage PRNG every time the area is
// picked (no stickiness across area runs).
void area_dynamic_gateway_init(struct area_state_s* a, uint16_t stage, uint32_t* prng);

bool area_dynamic_gateway_tick(struct world_state_s* w, struct area_state_s* a, float dz);
