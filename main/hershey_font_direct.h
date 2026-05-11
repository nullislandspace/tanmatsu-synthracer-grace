// Hershey Vector Font - Direct pixel rendering into pax_buf_t
// Bypasses PAX drawing functions for maximum speed.
// Based on the Hershey Simplex font from paulbourke.net/dataformats/hershey/
// Public domain

#ifndef HERSHEY_FONT_DIRECT_H
#define HERSHEY_FONT_DIRECT_H

#include <stdlib.h>
#include "pax_gfx.h"
#include "hershey.h"

// Font metrics
#define HERSHEY_DIRECT_BASE_HEIGHT 21

// Pre-computed color in the buffer's native pixel format. Built once
// per draw call (string or char) so the per-pixel write skips any
// format conversion. Holds both the 24bpp byte triple and the 16bpp
// packed halfword; set_pixel picks the right one based on buf->type.
typedef struct {
    uint8_t  r, g, b;     // RGB888 byte triple (native channel order)
    uint16_t packed565;   // RGB565 packed, already byte-swapped if needed
} hershey_native_color_t;

static inline hershey_native_color_t hershey_pack_color(pax_buf_t const *buf, pax_col_t color) {
    hershey_native_color_t c;
    c.r = (color >> 16) & 0xFF;
    c.g = (color >>  8) & 0xFF;
    c.b =  color        & 0xFF;
    uint16_t p565 = (uint16_t)(((c.r & 0xF8) << 8) | ((c.g & 0xFC) << 3) | (c.b >> 3));
    c.packed565 = buf->reverse_endianness ? (uint16_t)__builtin_bswap16(p565) : p565;
    return c;
}

// Set a single pixel directly in the pax_buf_t raw buffer.
// Supports PAX_BUF_24_888RGB and PAX_BUF_16_565RGB.
// Coordinates are in LOGICAL space (post-orientation).
static inline void hershey_direct_set_pixel(pax_buf_t *buf, int lx, int ly,
                                            hershey_native_color_t const *c) {
    // Raw (physical) buffer dimensions
    int raw_w = buf->width;
    int raw_h = buf->height;
    int rx, ry;

    // Convert logical coordinates to raw buffer coordinates
    switch (buf->orientation) {
        default:
        case PAX_O_UPRIGHT:
            rx = lx;
            ry = ly;
            break;
        case PAX_O_ROT_CCW:
            rx = ly;
            ry = raw_h - 1 - lx;
            break;
        case PAX_O_ROT_HALF:
            rx = raw_w - 1 - lx;
            ry = raw_h - 1 - ly;
            break;
        case PAX_O_ROT_CW:
            rx = raw_w - 1 - ly;
            ry = lx;
            break;
    }

    if (rx < 0 || rx >= raw_w || ry < 0 || ry >= raw_h) {
        return;
    }

    int const idx = ry * raw_w + rx;

    if (buf->type == PAX_BUF_16_565RGB) {
        buf->buf_16bpp[idx] = c->packed565;
    } else {
        // Assume 24bpp RGB888 — the only other format this app uses.
        uint8_t *pixels    = buf->buf_8bpp;
        int      byte_idx  = idx * 3;
        if (buf->reverse_endianness) {
            pixels[byte_idx + 0] = c->r;
            pixels[byte_idx + 1] = c->g;
            pixels[byte_idx + 2] = c->b;
        } else {
            pixels[byte_idx + 0] = c->b;
            pixels[byte_idx + 1] = c->g;
            pixels[byte_idx + 2] = c->r;
        }
    }
}

// Draw a line using Bresenham's algorithm (logical coordinates)
static inline void hershey_direct_draw_line(pax_buf_t *buf,
                                            int x0, int y0, int x1, int y1,
                                            hershey_native_color_t const *c) {
    int dx = abs(x1 - x0);
    int dy = abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    while (1) {
        hershey_direct_set_pixel(buf, x0, y0, c);

        if (x0 == x1 && y0 == y1) break;

        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

// Draw a single character
// Returns: scaled character width for horizontal advance
static inline int hershey_direct_draw_char(pax_buf_t *buf,
                                           hershey_native_color_t const *c,
                                           float screen_x, float screen_y, char ch, float font_height) {
    float scale = font_height / HERSHEY_DIRECT_BASE_HEIGHT;
    int idx = (int)ch - 32;
    if (idx < 0 || idx >= 95) {
        return (int)(16 * scale);
    }

    const int *glyph = simplex[idx];
    int num_vertices = glyph[0];
    int char_width = glyph[1];

    if (num_vertices == 0) {
        return (int)(char_width * scale);
    }

    int pen_down = 0;
    int prev_sx = 0, prev_sy = 0;

    for (int i = 0; i < num_vertices; i++) {
        int vx = glyph[2 + i * 2];
        int vy = glyph[2 + i * 2 + 1];

        if (vx == -1 && vy == -1) {
            pen_down = 0;
            continue;
        }

        int gx = (int)(vx * scale);
        int gy = (int)((HERSHEY_DIRECT_BASE_HEIGHT - vy) * scale);

        int sx = (int)screen_x + gx;
        int sy = (int)screen_y + gy;

        if (pen_down) {
            hershey_direct_draw_line(buf, prev_sx, prev_sy, sx, sy, c);
        }

        prev_sx = sx;
        prev_sy = sy;
        pen_down = 1;
    }

    return (int)(char_width * scale);
}

// Draw a string, returns pax_vec2f with (width, height)
static inline pax_vec2f hershey_direct_draw_string(pax_buf_t *buf, pax_col_t color,
                                                   float screen_x, float screen_y,
                                                   const char *str, float font_height) {
    hershey_native_color_t const c = hershey_pack_color(buf, color);
    float start_x = screen_x;
    while (*str) {
        screen_x += hershey_direct_draw_char(buf, &c, screen_x, screen_y, *str, font_height);
        str++;
    }
    return (pax_vec2f){screen_x - start_x, font_height};
}

// Calculate string width without drawing
static inline pax_vec2f hershey_direct_string_size(float font_height, const char *str) {
    float scale = font_height / HERSHEY_DIRECT_BASE_HEIGHT;
    int width = 0;
    while (*str) {
        int idx = (int)*str - 32;
        if (idx >= 0 && idx < 95) {
            width += (int)(simplex[idx][1] * scale);
        } else {
            width += (int)(16 * scale);
        }
        str++;
    }
    return (pax_vec2f){(float)width, font_height};
}

#endif // HERSHEY_FONT_DIRECT_H
