#pragma once

struct world_state_s;

// Green marker posts that line the side walls of a rest area, making
// it visually obvious the player is in the between-stage breather.
// Modelled on the bridges-area pillars (objects/bridge.c) but green,
// half the height, and with no connecting span slab.
//
// `z` is the world-z of the pillar pair's centre. Spawns 2 pool
// entries (one post per side wall); on a full pool either may fail
// to spawn and the caller need not handle it.
void rest_pillar_pair_spawn(struct world_state_s* w, float z);

// Post geometry. Footprint matches the bridge pillar (same width as
// the side wall, one wall segment deep); height is half the bridge
// pillar's BRIDGE_PILLAR_HEIGHT (3.0).
#define REST_PILLAR_HALF_D   1.5f
#define REST_PILLAR_HEIGHT   1.5f
