#ifndef KLYE_FONT_H
#define KLYE_FONT_H

#include <stdbool.h>
#include <stdint.h>

#include "gfx.h"

#define FONT_GLYPH_WIDTH 8
#define FONT_GLYPH_HEIGHT 8
#define FONT_ASCENT 6
#define FONT_DESCENT 2
#define FONT_LINE_HEIGHT (FONT_ASCENT + FONT_DESCENT)
#define FONT_ADVANCE 8
#define FONT_MIN_SCALE 1
#define FONT_MAX_SCALE 4

uint8_t font_glyph(char character, int row);
int font_char_count(const char *text);
int font_text_width(const char *text, int scale);
int font_text_width_spaced(const char *text, int scale, int spacing);
int font_line_height(int scale);
void font_draw(struct gfx_surface *surface, int x, int baseline,
               const char *text, uint32_t color, int scale);
void font_draw_spaced(struct gfx_surface *surface, int x, int baseline,
                      const char *text, uint32_t color, int scale,
                      int spacing);
void font_draw_centered(struct gfx_surface *surface, int center_x, int baseline,
                        const char *text, uint32_t color, int scale);
/* Centered text with a drop shadow behind it, for text drawn over a background
 * the caller does not control. */
void font_draw_centered_shadow(struct gfx_surface *surface, int center_x,
                               int baseline, const char *text, uint32_t color,
                               int scale);
void font_draw_right(struct gfx_surface *surface, int right_x, int baseline,
                     const char *text, uint32_t color, int scale);
void font_draw_shadow(struct gfx_surface *surface, int x, int baseline,
                      const char *text, uint32_t color, uint32_t shadow_color,
                      int scale, int offset);

#endif
