// Persistent metallic scrape SFX. Triggered while the ship is in
// glancing contact with an object or border wall; stopped when
// contact ends. `intensity` is in [0, 1] and modulates both the
// amplitude and the filter resonance — light brushes are quiet
// and dull, heavy contact is loud and sharp.

#pragma once

#include <stdbool.h>

bool sfx_scrape_start(void);
void sfx_scrape_stop(void);
void sfx_scrape_set_intensity(float intensity);
