#pragma once

struct world_state_s;

// Rest-area wall markers: a grey tapered hex post topped by a green
// beacon sphere that pulses (full green → grey → full, ~1 s). They line
// the side-wall tops to flag the between-stage breather, replacing the
// old green cube posts. The model lives in restarea_marker_model.h
// (generated from openscad/restarea_markers.3mf by
// tools/object_3mf_to_header.py).
//
// `z` is the world-z of the pair's centre. Spawns one marker on each
// side wall; on a full pool either may fail to spawn and the caller need
// not handle it.
void restarea_marker_pair_spawn(struct world_state_s* w, float z);
