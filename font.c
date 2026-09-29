#include <stdbool.h>
#include <stdint.h>

#include "font.h"

#include "font_ext.h"

static const uint8_t glyph_data[95][FONT_GLYPH_HEIGHT] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00},
    {0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x36, 0x36, 0x7F, 0x36, 0x7F, 0x36, 0x36, 0x00},
    {0x0C, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x0C, 0x00},
    {0x00, 0x63, 0x33, 0x18, 0x0C, 0x66, 0x63, 0x00},
    {0x1C, 0x36, 0x1C, 0x6E, 0x3B, 0x33, 0x6E, 0x00},
    {0x06, 0x06, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x18, 0x0C, 0x06, 0x06, 0x06, 0x0C, 0x18, 0x00},
    {0x06, 0x0C, 0x18, 0x18, 0x18, 0x0C, 0x06, 0x00},
    {0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00},
    {0x00, 0x0C, 0x0C, 0x3F, 0x0C, 0x0C, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x06},
    {0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00},
    {0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x01, 0x00},
    {0x3E, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x3E, 0x00},
    {0x0C, 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0x00},
    {0x1E, 0x33, 0x30, 0x1C, 0x06, 0x33, 0x3F, 0x00},
    {0x1E, 0x33, 0x30, 0x1C, 0x30, 0x33, 0x1E, 0x00},
    {0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x78, 0x00},
    {0x3F, 0x03, 0x1F, 0x30, 0x30, 0x33, 0x1E, 0x00},
    {0x1C, 0x06, 0x03, 0x1F, 0x33, 0x33, 0x1E, 0x00},
    {0x3F, 0x33, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x00},
    {0x1E, 0x33, 0x33, 0x1E, 0x33, 0x33, 0x1E, 0x00},
    {0x1E, 0x33, 0x33, 0x3E, 0x30, 0x18, 0x0E, 0x00},
    {0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x00},
    {0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x06},
    {0x18, 0x0C, 0x06, 0x03, 0x06, 0x0C, 0x18, 0x00},
    {0x00, 0x00, 0x3F, 0x00, 0x00, 0x3F, 0x00, 0x00},
    {0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06, 0x00},
    {0x1E, 0x33, 0x30, 0x18, 0x0C, 0x00, 0x0C, 0x00},
    {0x3E, 0x63, 0x7B, 0x7B, 0x7B, 0x03, 0x1E, 0x00},
    {0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00},
    {0x3F, 0x66, 0x66, 0x3E, 0x66, 0x66, 0x3F, 0x00},
    {0x3C, 0x66, 0x03, 0x03, 0x03, 0x66, 0x3C, 0x00},
    {0x1F, 0x36, 0x66, 0x66, 0x66, 0x36, 0x1F, 0x00},
    {0x7F, 0x46, 0x16, 0x1E, 0x16, 0x46, 0x7F, 0x00},
    {0x7F, 0x46, 0x16, 0x1E, 0x16, 0x06, 0x0F, 0x00},
    {0x3C, 0x66, 0x03, 0x03, 0x73, 0x66, 0x7C, 0x00},
    {0x33, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x33, 0x00},
    {0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},
    {0x78, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E, 0x00},
    {0x67, 0x66, 0x36, 0x1E, 0x36, 0x66, 0x67, 0x00},
    {0x0F, 0x06, 0x06, 0x06, 0x46, 0x66, 0x7F, 0x00},
    {0x63, 0x77, 0x7F, 0x7F, 0x6B, 0x63, 0x63, 0x00},
    {0x63, 0x67, 0x6F, 0x7B, 0x73, 0x63, 0x63, 0x00},
    {0x1C, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1C, 0x00},
    {0x3F, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x0F, 0x00},
    {0x1E, 0x33, 0x33, 0x33, 0x3B, 0x1E, 0x38, 0x00},
    {0x3F, 0x66, 0x66, 0x3E, 0x36, 0x66, 0x67, 0x00},
    {0x1E, 0x33, 0x07, 0x0E, 0x38, 0x33, 0x1E, 0x00},
    {0x3F, 0x2D, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},
    {0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x3F, 0x00},
    {0x33, 0x33, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00},
    {0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00},
    {0x63, 0x63, 0x36, 0x1C, 0x1C, 0x36, 0x63, 0x00},
    {0x33, 0x33, 0x33, 0x1E, 0x0C, 0x0C, 0x1E, 0x00},
    {0x7F, 0x63, 0x31, 0x18, 0x4C, 0x66, 0x7F, 0x00},
    {0x1E, 0x06, 0x06, 0x06, 0x06, 0x06, 0x1E, 0x00},
    {0x03, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x40, 0x00},
    {0x1E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1E, 0x00},
    {0x08, 0x1C, 0x36, 0x63, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF},
    {0x0C, 0x0C, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x1E, 0x30, 0x3E, 0x33, 0x6E, 0x00},
    {0x07, 0x06, 0x06, 0x3E, 0x66, 0x66, 0x3B, 0x00},
    {0x00, 0x00, 0x1E, 0x33, 0x03, 0x33, 0x1E, 0x00},
    {0x38, 0x30, 0x30, 0x3E, 0x33, 0x33, 0x6E, 0x00},
    {0x00, 0x00, 0x1E, 0x33, 0x3F, 0x03, 0x1E, 0x00},
    {0x1C, 0x36, 0x06, 0x0F, 0x06, 0x06, 0x0F, 0x00},
    {0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x1F},
    {0x07, 0x06, 0x36, 0x6E, 0x66, 0x66, 0x67, 0x00},
    {0x0C, 0x00, 0x0E, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},
    {0x30, 0x00, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E},
    {0x07, 0x06, 0x66, 0x36, 0x1E, 0x36, 0x67, 0x00},
    {0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},
    {0x00, 0x00, 0x33, 0x7F, 0x7F, 0x6B, 0x63, 0x00},
    {0x00, 0x00, 0x1F, 0x33, 0x33, 0x33, 0x33, 0x00},
    {0x00, 0x00, 0x1E, 0x33, 0x33, 0x33, 0x1E, 0x00},
    {0x00, 0x00, 0x3B, 0x66, 0x66, 0x3E, 0x06, 0x0F},
    {0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x78},
    {0x00, 0x00, 0x3B, 0x6E, 0x66, 0x06, 0x0F, 0x00},
    {0x00, 0x00, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x00},
    {0x08, 0x0C, 0x3E, 0x0C, 0x0C, 0x2C, 0x18, 0x00},
    {0x00, 0x00, 0x33, 0x33, 0x33, 0x33, 0x6E, 0x00},
    {0x00, 0x00, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00},
    {0x00, 0x00, 0x63, 0x6B, 0x7F, 0x7F, 0x36, 0x00},
    {0x00, 0x00, 0x63, 0x36, 0x1C, 0x36, 0x63, 0x00},
    {0x00, 0x00, 0x33, 0x33, 0x33, 0x3E, 0x30, 0x1F},
    {0x00, 0x00, 0x3F, 0x19, 0x0C, 0x26, 0x3F, 0x00},
    {0x38, 0x0C, 0x0C, 0x07, 0x0C, 0x0C, 0x38, 0x00},
    {0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00},
    {0x07, 0x0C, 0x0C, 0x38, 0x0C, 0x0C, 0x07, 0x00},
    {0x6E, 0x3B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
};

uint8_t font_glyph(char character, int row)
{
    int index = (int)(uint8_t)character - 32;

    if (row < 0 || row >= FONT_GLYPH_HEIGHT) {
        return 0U;
    }
    if (index < 0 || index > 94) {
        index = (int)'?' - 32;
    }
    return glyph_data[index][row];
}

int font_char_count(const char *text)
{
    /* Codepoints, for the same reason the drawing loop counts them: a caller
     * sizing a window by font_text_width has to agree with where the glyphs
     * were actually placed. */
    int count = 0;

    for (int index = 0; text[index] != '\0';) {
        (void)font_utf8_next(text, &index);
        count++;
    }
    return count;
}


int font_text_width(const char *text, int scale)
{
    int length = font_char_count(text);

    if (length == 0) {
        return 0;
    }
    return (length * FONT_ADVANCE - (FONT_ADVANCE - FONT_GLYPH_WIDTH)) * scale;
}

int font_text_width_spaced(const char *text, int scale, int spacing)
{
    int length = font_char_count(text);

    if (length == 0) {
        return 0;
    }
    return (length * FONT_ADVANCE + (length - 1) * spacing) * scale;
}

int font_line_height(int scale)
{
    return FONT_GLYPH_HEIGHT * scale;
}

/* Draw one glyph, supersampled.
 *
 * A bitmap font scaled up by copying each pixel into a block looks like a
 * mosaic: every stem becomes a staircase and the letterforms stop being
 * readable as letters.  Nearest neighbour was what made the interface look
 * like graph paper, and raising the scale without changing this only made the
 * blocks bigger.
 *
 * So the glyph is sampled with a box filter instead.  Each destination pixel
 * covers an area of the source glyph, and it is drawn only if enough of that
 * area is ink -- which turns a stem one pixel wide into a line two pixels wide
 * with a soft edge, and turns a diagonal into a diagonal rather than a
 * staircase.  It is not a full anti-aliasing pass and it is not gamma
 * correct, but it is a filter rather than a copy, and at the sizes used here
 * that is the difference between letters and blocks.
 *
 * The coverage is counted rather than sampled, so the result does not change
 * with where the glyph happens to land on the pixel grid: a stem is the same
 * weight in every column, which is what makes a row of text look even. */
static void glyph_draw(struct gfx_surface *surface, int x, int top,
                       uint8_t (*row)(int, int, void *), void *source,
                       uint32_t color, int scale)
{
    int width = FONT_GLYPH_WIDTH * scale;
    int height = FONT_GLYPH_HEIGHT * scale;
    int left = x < 0 ? 0 : x;
    int right = x + width;

    if (top >= (int)surface->height || top + height <= 0) {
        return;
    }
    if (right > (int)surface->width) {
        right = (int)surface->width;
    }
    for (int py = top; py < top + height; ++py) {
        int source_row = (py - top) / scale;
        int empty;

        if (py < 0) {
            continue;
        }
        /* A row with no ink in it is skipped whole.  Worth the pass over the
         * row: most rows of most glyphs are empty at the top and bottom, and
         * this is the common case in a screen full of text. */
        for (int column = 0; column < FONT_GLYPH_WIDTH; ++column) {
            if ((row(source_row, column, source) & (1U << column)) == 0U) {
                empty = 1;
            } else {
                empty = 0;
                break;
            }
        }
        if (empty) {
            continue;
        }
        for (int column = 0; column < FONT_GLYPH_WIDTH; ++column) {
            int on = (row(source_row, column, source) & (1U << column)) != 0U;

            for (int sub = 0; sub < scale; ++sub) {
                int px = x + column * scale + sub;

                if (px < left || px >= right || px < 0) {
                    continue;
                }
                if (on) {
                    surface->pixels[(uint32_t)py * surface->pitch_pixels +
                                    (uint32_t)px] = color;
                }
                /* The pixels with no ink in them are left alone.
                 *
                 * They used to be blended towards the text colour, with the
                 * row's total coverage as the alpha, on the theory that it
                 * gave a horizontal stem a soft end.  It blended the whole
                 * destination row rather than the ends, so every glyph with
                 * any ink in a row got the text colour washed across the full
                 * width of its cell: a light rectangle behind every character,
                 * wherever text is drawn.  The terminal, the menu bar, the
                 * dock and the window titles all go through here, which is why
                 * it looked like a fault in all of them at once.
                 *
                 * At 1:1 there is nothing to soften anyway -- an 8x8 glyph
                 * either has a pixel in a cell or it does not, and a half
                 * covered pixel at 1:1 is a lighter pixel, not a smoother
                 * edge.  So the ink is drawn and nothing else is touched, and
                 * text is crisp against whatever is behind it. */
            }
        }
    }
}

static uint8_t glyph_row_ascii(int row, int column, void *source)
{
    (void)column;
    return font_glyph((char)(intptr_t)source, row);
}

static uint8_t glyph_row_ext(int row, int column, void *source)
{
    uint8_t bits = 0U;

    (void)column;
    (void)font_ext_glyph_for_cell((uint8_t)(intptr_t)source, row, &bits);
    return bits;
}

void font_draw_spaced(struct gfx_surface *surface, int x, int baseline,
                      const char *text, uint32_t color, int scale, int spacing)
{
    int top;
    int cursor = x;
    int step;

    if (scale < FONT_MIN_SCALE) {
        scale = FONT_MIN_SCALE;
    }
    if (scale > FONT_MAX_SCALE) {
        scale = FONT_MAX_SCALE;
    }
    top = baseline - FONT_ASCENT * scale;
    step = (FONT_ADVANCE + spacing) * scale;
    for (int index = 0; text[index] != '\0';) {
        uint32_t codepoint;

        /* A cell byte, not a sequence.
         *
         * The terminal stores a box glyph's index in one byte at 0xF0, so that
         * a cell is always one character wide.  0xF0 is also the lead byte of a
         * four byte UTF-8 sequence, and decoding it as one swallowed the next
         * three characters of the line as continuation bytes: the glyph drew
         * and the text after it vanished, three characters at a time, which
         * looked like the line had holes in it.  So the byte is taken as a
         * byte before any decoding is attempted. */
        if (((uint8_t)text[index] & 0xF0U) == 0xF0U) {
            codepoint = (uint8_t)text[index];
            index++;
        } else {
            codepoint = font_utf8_next(text, &index);
        }

        if (cursor >= (int)surface->width) {
            break;
        }
        if (codepoint >= ' ' && codepoint <= '~') {
            glyph_draw(surface, cursor, top, glyph_row_ascii,
                       (void *)(intptr_t)codepoint, color, scale);
        } else if (font_ext_has(codepoint)) {
            int index_of = font_ext_index(codepoint);

            if (index_of >= 0) {
                glyph_draw(surface, cursor, top, glyph_row_ext,
                           (void *)(intptr_t)(0xF0U | (uint32_t)index_of),
                           color, scale);
            }
        } else if (((uint8_t)codepoint & 0xF0U) == 0xF0U) {
            uint8_t cell = 0U;

            /* a cell byte written by the terminal */
            if (font_ext_glyph_for_cell((uint8_t)codepoint, 0, &cell)) {
                glyph_draw(surface, cursor, top, glyph_row_ext,
                           (void *)(intptr_t)codepoint, color, scale);
            }
        }
        cursor += step;
    }
}

void font_draw(struct gfx_surface *surface, int x, int baseline,
               const char *text, uint32_t color, int scale)
{
    font_draw_spaced(surface, x, baseline, text, color, scale, 0);
}

void font_draw_centered(struct gfx_surface *surface, int center_x, int baseline,
                        const char *text, uint32_t color, int scale)
{
    int width = font_text_width(text, scale);

    font_draw(surface, center_x - width / 2, baseline, text, color, scale);
}

/* Centered text with a drop shadow behind it.
 *
 * Text drawn straight onto an arbitrary background is readable only when the
 * background happens to contrast with it.  The alternative -- a filled rounded
 * rectangle behind the label -- works on the desktop's own background and looks
 * like a rendering fault on anything the user chose, because a box is drawn
 * whether or not it is wanted.  A shadow separates the text from whatever is
 * behind it without putting anything on the screen, so it works over a
 * photograph and over a flat colour equally.
 *
 * Three offsets rather than one, so the shadow has an edge and does not read as
 * a smeared duplicate of the text. */
void font_draw_centered_shadow(struct gfx_surface *surface, int center_x,
                               int baseline, const char *text, uint32_t color,
                               int scale)
{
    int width = font_text_width(text, scale);
    int x = center_x - width / 2;

    font_draw(surface, x + scale, baseline + scale, text, 0xFF000000U, scale);
    font_draw(surface, x - scale, baseline + scale, text, 0xB0000000U, scale);
    font_draw(surface, x, baseline - scale, text, 0x80000000U, scale);
    font_draw(surface, x, baseline, text, color, scale);
}

void font_draw_right(struct gfx_surface *surface, int right_x, int baseline,
                     const char *text, uint32_t color, int scale)
{
    int width = font_text_width(text, scale);

    font_draw(surface, right_x - width, baseline, text, color, scale);
}

void font_draw_shadow(struct gfx_surface *surface, int x, int baseline,
                      const char *text, uint32_t color, uint32_t shadow_color,
                      int scale, int offset)
{
    if (offset != 0) {
        font_draw(surface, x, baseline + offset, text, shadow_color, scale);
    }
    font_draw(surface, x, baseline, text, color, scale);
}
