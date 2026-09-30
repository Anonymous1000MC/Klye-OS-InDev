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
/* One destination pixel's coverage from a glyph, 0 to 255.
 *
 * Bilinear.  The destination pixel's centre is mapped back into source space
 * and the four neighbours around it are weighted by how far it sits between
 * them, so the result follows the glyph's shape rather than the pixel grid.
 * Nearest neighbour, which is what this replaced, copies one source pixel into
 * a scale by scale block: a stem one pixel wide becomes a staircase, and at
 * 8x the underlying bitmap is visible as tiles.  That is the whole of what
 * made the interface look like graph paper.
 *
 * Fixed point in 8.8 rather than floating point, because this runs per
 * destination pixel over every glyph on the screen and there is no FPU here to
 * do it with.  The vertical divisor is the destination extent, not the source
 * height: getting that wrong makes the sample walk off the bottom of the glyph
 * and every descender disappears, which is what the preview caught.
 *
 * At 1:1 the weights collapse to a single source pixel, so this is exactly the
 * old behaviour with no smoothing -- sharp text at its native size, which is
 * what a crisp bitmap font should be. */
static unsigned glyph_coverage(uint8_t (*row)(int, int, void *), void *source,
                               int dx, int dy, int dest_w, int dest_h)
{
    /* Centre of this destination pixel, mapped back into source space, in 8.8
     * fixed point:  (destination + 0.5) * source_extent / destination_extent
     * - 0.5.
     *
     * The half-pixel offset is why the weights are not symmetric at the very
     * edge of a glyph: the first destination column has its centre half a
     * destination pixel in, which lands it between source column -1 and 0, so
     * it picks up a fraction of the one next door.  That is what rounds off a
     * stem rather than leaving it square, and it is why the out of range case
     * clamps instead of returning nothing.
     *
     * At 1:1 every position comes out as an exact integer with no fraction, so
     * the weights collapse to the single source pixel and the result is exactly
     * the old hard-edged glyph.  Two earlier forms of this were wrong in ways
     * that only showed up when looked at: a flat -128 was right at one scale
     * and not another, and shifting the destination position before dividing
     * overflowed the product and put the sample several rows down the glyph. */
    int sx = ((((2 * dx + 1) * FONT_GLYPH_WIDTH * 256) / (2 * dest_w)) - 128);
    int sy = ((((2 * dy + 1) * FONT_GLYPH_HEIGHT * 256) / (2 * dest_h)) - 128);
    int x0 = sx >> 8;
    int y0 = sy >> 8;
    int fx = sx & 0xFF;
    int fy = sy & 0xFF;
    int w00 = (256 - fx) * (256 - fy);
    int w10 = fx * (256 - fy);
    int w01 = (256 - fx) * fy;
    int w11 = fx * fy;
    /* Outside the glyph, treat the edge pixel as its own neighbour rather than
     * dropping the sample.  The first and last destination columns of a glyph
     * have their centres between the edge and the one next door, and returning
     * nothing for them is what leaves a stem square at both ends while the
     * middle of it is soft. */
    int x1 = x0 + 1;
    int y1 = y0 + 1;

    if (x0 < 0) {
        x0 = 0;
    } else if (x0 >= FONT_GLYPH_WIDTH) {
        x0 = FONT_GLYPH_WIDTH - 1;
    }
    if (y0 < 0) {
        y0 = 0;
    } else if (y0 >= FONT_GLYPH_HEIGHT) {
        y0 = FONT_GLYPH_HEIGHT - 1;
    }
    if (x1 < 0) {
        x1 = 0;
    } else if (x1 >= FONT_GLYPH_WIDTH) {
        x1 = FONT_GLYPH_WIDTH - 1;
    }
    if (y1 < 0) {
        y1 = 0;
    } else if (y1 >= FONT_GLYPH_HEIGHT) {
        y1 = FONT_GLYPH_HEIGHT - 1;
    }

    /* Weights are unchanged by the clamp, so a sample sitting half outside the
     * glyph keeps the fraction it had: the edge pixel contributes what share of
     * it falls inside.  That is what rounds the stem off. */
    {
        unsigned total = (unsigned)((((row(y0, x0, source) & (1U << x0)) != 0U)
                                     ? w00 : 0) +
                                    (((row(y0, x1, source) & (1U << x1)) != 0U)
                                     ? w10 : 0) +
                                    (((row(y1, x0, source) & (1U << x0)) != 0U)
                                     ? w01 : 0) +
                                    (((row(y1, x1, source) & (1U << x1)) != 0U)
                                     ? w11 : 0) +
                                    128) >> 8;

        /* Clamped neighbours can both be the same edge pixel, so the four
         * weights can sum past 65536 and the result can come out at 256.  An
         * alpha of 256 would be scaled by 256 and over-saturate whatever it is
         * drawn over, so it is clamped here rather than trusted. */
        return total > 255U ? 255U : total;
    }
}

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
        int dy = py - top;

        if (py < 0) {
            continue;
        }
        for (int dx = 0; dx < width; ++dx) {
            int px = x + dx;
            unsigned coverage;

            if (px < left || px >= right || px < 0) {
                continue;
            }
            coverage = glyph_coverage(row, source, dx, dy, width, height);
            if (coverage == 0U) {
                continue;
            }
            /* Blended, not written.
             *
             * Coverage is a number between 0 and 255 rather than a bit, so it
             * has to be blended: writing the text colour outright would paint
             * every destination pixel in the cell, including the ones with
             * almost no ink, and put a solid rectangle behind every glyph.
             * That was a bug here once and it is why the empty pixels were
             * left alone instead -- which was right for a hard-edged bitmap
             * and wrong for a filtered one, since a filtered glyph has no
             * "empty" pixels left, only faint ones. */
            gfx_blend_pixel(surface, px, py, color, coverage);
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
