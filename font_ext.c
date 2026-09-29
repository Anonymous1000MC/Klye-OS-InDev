/* font_ext.c - glyphs for the box drawing characters, and the UTF-8 decoder
 * that reaches them.
 *
 * The main table in font.c covers printable ASCII, 32 to 126, and everything
 * outside that draws nothing.  The box drawing characters are three bytes each
 * in UTF-8, so a logo built from them showed as blank space: not a question of
 * a missing glyph but of three bytes per character, none of which is a
 * character.
 *
 * So they are added here, as a small table addressed by codepoint, and the
 * decode is done in one place that both the font and the terminal use.  A
 * terminal and a font that decoded UTF-8 separately would disagree about how
 * many cells a string occupies, and the text would drift sideways.
 */

#include "font_ext.h"

/* Eight rows of eight pixels, one bit per pixel, bit 0 leftmost.
 *
 * The double line characters put their verticals in columns 1 and 2 and 5 and
 * 6, and their horizontals in rows 1 and 2 and 5 and 6, so that a corner joins
 * the line it is meant to continue: a top left corner is a horizontal across
 * the top and a vertical down the left, and it is the same two bars as the
 * horizontal and the vertical, which is why they line up when placed next to
 * each other. */
static const uint8_t box_glyphs[FONT_EXT_GLYPH_COUNT][FONT_GLYPH_HEIGHT] = {
    /* U+2588 FULL BLOCK */
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
    /* U+2580 UPPER HALF BLOCK */
    {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00},
    /* U+2584 LOWER HALF BLOCK */
    {0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF},
    /* U+258C LEFT HALF BLOCK */
    {0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F},
    /* U+2590 RIGHT HALF BLOCK */
    {0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0},
    /* U+2550 BOX DRAWINGS DOUBLE HORIZONTAL */
    {0x00, 0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF, 0x00},
    /* U+2551 BOX DRAWINGS DOUBLE VERTICAL */
    {0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66},
    /* U+2554 BOX DRAWINGS DOUBLE DOWN AND RIGHT */
    {0x00, 0xFF, 0xFF, 0x66, 0x66, 0x66, 0x66, 0x66},
    /* U+2557 BOX DRAWINGS DOUBLE DOWN AND LEFT */
    {0x00, 0xFF, 0xFF, 0x99, 0x99, 0x99, 0x99, 0x99},
    /* U+255A BOX DRAWINGS DOUBLE UP AND RIGHT */
    {0x66, 0x66, 0x66, 0x66, 0x66, 0xFF, 0xFF, 0x00},
    /* U+255D BOX DRAWINGS DOUBLE UP AND LEFT */
    {0x99, 0x99, 0x99, 0x99, 0x99, 0xFF, 0xFF, 0x00},
    /* U+2551-ish light horizontal, for the single line set */
    {0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00},
    /* U+2502 light vertical */
    {0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18},
    /* U+250C light down and right */
    {0x00, 0x00, 0x1F, 0x18, 0x18, 0x18, 0x18, 0x18},
    /* U+2510 light down and left */
    {0x00, 0x00, 0xF8, 0x18, 0x18, 0x18, 0x18, 0x18},
    /* U+2514 light up and right */
    {0x18, 0x18, 0x18, 0x18, 0x18, 0x1F, 0x00, 0x00},
    /* U+2518 light up and left */
    {0x18, 0x18, 0x18, 0x18, 0x18, 0xF8, 0x00, 0x00},
    /* U+2591 LIGHT SHADE, for a progress bar or a filled cell */
    {0x88, 0x22, 0x88, 0x22, 0x88, 0x22, 0x88, 0x22},
    /* U+2592 MEDIUM SHADE */
    {0xDD, 0x77, 0xDD, 0x77, 0xDD, 0x77, 0xDD, 0x77},
    /* U+2593 DARK SHADE */
    {0xFF, 0x77, 0xFF, 0x77, 0xFF, 0x77, 0xFF, 0x77}
};

static const uint32_t box_codepoints[FONT_EXT_GLYPH_COUNT] = {
    0x2588, 0x2580, 0x2584, 0x258C, 0x2590,
    0x2550, 0x2551, 0x2554, 0x2557, 0x255A, 0x255D,
    0x2500, 0x2502, 0x250C, 0x2510, 0x2514, 0x2518,
    0x2591, 0x2592, 0x2593
};

int font_ext_index(uint32_t codepoint)
{
    for (uint32_t index = 0; index < FONT_EXT_GLYPH_COUNT; ++index) {
        if (box_codepoints[index] == codepoint) {
            return (int)index;
        }
    }
    return -1;
}

bool font_ext_glyph_for_cell(uint8_t cell, int row, uint8_t *bits)
{
    uint32_t index;

    if ((cell & 0xF0U) != FONT_EXT_CELL_BASE) {
        return false;
    }
    index = (uint32_t)(cell & 0x0FU);
    if (index >= FONT_EXT_GLYPH_COUNT) {
        return false;
    }
    if (row < 0 || row >= FONT_GLYPH_HEIGHT) {
        *bits = 0U;
        return true;
    }
    *bits = box_glyphs[index][row];
    return true;
}

bool font_ext_glyph(uint32_t codepoint, int row, uint8_t *bits)
{
    for (uint32_t index = 0; index < FONT_EXT_GLYPH_COUNT; ++index) {
        if (box_codepoints[index] != codepoint) {
            continue;
        }
        if (row < 0 || row >= FONT_GLYPH_HEIGHT) {
            *bits = 0U;
            return true;
        }
        *bits = box_glyphs[index][row];
        return true;
    }
    return false;
}

bool font_ext_has(uint32_t codepoint)
{
    for (uint32_t index = 0; index < FONT_EXT_GLYPH_COUNT; ++index) {
        if (box_codepoints[index] == codepoint) {
            return true;
        }
    }
    return false;
}

uint32_t font_utf8_next(const char *text, int *index)
{
    const uint8_t *bytes = (const uint8_t *)(const void *)text;
    uint8_t first;
    uint32_t value;
    int extra;
    int at = *index;

    first = bytes[at];
    if (first == 0U) {
        *index = at;
        return 0U;
    }
    if (first < 0x80U) {
        *index = at + 1;
        return first;
    }
    /* Four forms, distinguished by the top bits: 0xxxxxxx is the one byte
     * case above, 110xxxxx adds one, 1110xxxx adds two, 11110xxx adds three.
     * Anything else is not a sequence at all and is reported as the replacement
     * character rather than being decoded from whatever follows it, since a
     * continuation byte on its own is not text. */
    if ((first & 0xE0U) == 0xC0U) {
        extra = 1;
        value = (uint32_t)(first & 0x1FU);
    } else if ((first & 0xF0U) == 0xE0U) {
        extra = 2;
        value = (uint32_t)(first & 0x0FU);
    } else if ((first & 0xF8U) == 0xF0U) {
        extra = 3;
        value = (uint32_t)(first & 0x07U);
    } else {
        *index = at + 1;
        return 0xFFFDU;
    }
    at++;
    for (int step = 0; step < extra; ++step) {
        if ((bytes[at] & 0xC0U) != 0x80U) {
            /* truncated: the sequence stops where it stops */
            *index = at;
            return 0xFFFDU;
        }
        value = (value << 6) | (uint32_t)(bytes[at] & 0x3FU);
        at++;
    }
    *index = at;
    return value;
}
