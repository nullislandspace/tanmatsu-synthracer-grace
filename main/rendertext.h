#pragma once

#include "pax_gfx.h"
#include "pax_text.h"

// Uncomment to use Hershey vector font instead of PAX bitmap font
#define USE_HERSHEY_FONT

// Uncomment to use direct pixel operations instead of PAX drawing primitives
// (only effective when USE_HERSHEY_FONT is enabled)
#define USE_HERSHEY_DIRECT

pax_vec2f rendertext_draw(pax_buf_t *buf, pax_col_t color, pax_font_t const *font, float font_size, float x, float y, char const *text);
pax_vec2f rendertext_size(pax_font_t const *font, float font_size, char const *text);
