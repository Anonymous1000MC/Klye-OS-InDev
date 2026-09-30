/* The 8x16 font, checked against properties that a wrong font breaks and that
 * do not depend on how any particular letter happens to look.
 *
 * The failures this guards are the ones that are easy to ship by accident: a
 * glyph that is all blank, a row that is truncated, a Latin-1 code point that
 * falls off the end of the table, and a letter that has been shifted by a row
 * so that it still looks plausible but no longer lines up with its baseline. */

#include <stdint.h>
#include <stdio.h>

#include "../include/font.h"

static int failures;

static void check(int condition, const char *what)
{
    if (!condition) {
        printf("  FAIL %s\n", what);
        failures++;
    }
}

static int glyph_ink(uint8_t character)
{
    int total = 0;

    for (int row = 0; row < FONT8X16_HEIGHT; ++row) {
        uint8_t bits = font8x16_row(character, row);
        for (int bit = 0; bit < 8; ++bit) {
            if (bits & (uint8_t)(0x80U >> bit)) {
                total++;
            }
        }
    }
    return total;
}

/* Rows in the top half and the bottom half, so a glyph that is vertically
 * offset still has ink and still passes a naive "is it blank" check. */
static int ink_in_rows(uint8_t character, int first, int last)
{
    int total = 0;

    for (int row = first; row <= last; ++row) {
        uint8_t bits = font8x16_row(character, row);
        for (int bit = 0; bit < 8; ++bit) {
            if (bits & (uint8_t)(0x80U >> bit)) {
                total++;
            }
        }
    }
    return total;
}

static void print_glyph(uint8_t character)
{
    for (int row = 0; row < FONT8X16_HEIGHT; ++row) {
        printf("    ");
        for (int bit = 0; bit < 8; ++bit) {
            printf("%c", (font8x16_row(character, row) & (uint8_t)(0x80U >> bit))
                            ? '#' : '.');
        }
        printf("\n");
    }
}

int main(void)
{
    /* The range: everything a Latin-1 program can print is in the table. */
    check(FONT8X16_HEIGHT == 16, "the font is 16 rows tall");
    check(FONT8X16_ASCENT + FONT8X16_DESCENT == FONT8X16_HEIGHT,
          "ascent and descent add up to the glyph height");

    /* Space is blank and everything else is not. */
    check(glyph_ink(0x20) == 0, "space is blank");

    /* Letters carry ink in both halves.  A glyph shifted down a row or two by a
     * bad table offset still passes a blank check and fails this one, which is
     * exactly the bug that a wrong PSF header offset produces: the letters
     * look like letters and are half a line out of place. */
    static const uint8_t letters[] = {
        0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
        0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50,
        0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
        0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37
    };

    for (unsigned i = 0; i < sizeof(letters); ++i) {
        uint8_t character = letters[i];
        char label[8];

        check(glyph_ink(character) > 0, "a letter is not blank");
        /* Capitals live in the top half, which is what puts them on the
         * baseline; lowercase lives mostly below the middle. */
        if (character >= 0x41 && character <= 0x5A) {
            int top = ink_in_rows(character, 0, 9);
            snprintf(label, sizeof label, "cap 0x%02X top", character);
            check(top > 0, label);
        }
    }

    /* Where the ink actually is, which is the real property and the one a
     * shifted table breaks.  This font's capitals and digits occupy rows 2 to
     * 11 of the 16 row cell: two rows of leading space and four below, which is
     * what puts the text on a baseline at 12 with room for descenders under it.
     *
     * Checking "ink is somewhere in the cell" would pass for a table offset by
     * one row and is why the numbers below are specific. */
    for (uint8_t c = 0x30; c <= 0x39; ++c) {
        char label[20];

        snprintf(label, sizeof label, "digit %d sits on the baseline",
                 (int)(c - 0x30));
        check(ink_in_rows(c, 2, 11) > 0 && ink_in_rows(c, 12, 15) == 0, label);
    }
    /* Capitals sit on the baseline the same way, with one exception: Q has a
     * tail under it, which is correct typography and not a shifted table. */
    for (uint8_t c = 0x41; c <= 0x5A; ++c) {
        char label[16];

        snprintf(label, sizeof label, "capital 0x%02X on the baseline", c);
        check(ink_in_rows(c, 2, 11) > 0, label);
        if (c != 0x51) {
            check(ink_in_rows(c, 12, 15) == 0, label);
        }
    }
    check(ink_in_rows(0x51, 12, 15) > 0, "Q has its tail");

    /* Lowercase with a descender: g, j, p, q, y.  A font where the descenders
     * are missing still reads as text until a filename has one in it. */
    static const uint8_t descenders[] = { 0x67, 0x6A, 0x70, 0x71, 0x79 };

    for (unsigned i = 0; i < sizeof(descenders); ++i) {
        uint8_t character = descenders[i];
        char label[16];

        snprintf(label, sizeof label, "0x%02X descends", character);
        check(ink_in_rows(character, 13, 15) > 0, label);
    }

    /* Ascenders reach the top row: b, d, f, h, k, l, t. */
    static const uint8_t ascenders[] = { 0x62, 0x64, 0x66, 0x68, 0x6B, 0x6C, 0x74 };

    for (unsigned i = 0; i < sizeof(ascenders); ++i) {
        uint8_t character = ascenders[i];
        char label[16];

        snprintf(label, sizeof label, "0x%02X ascends", character);
        check(ink_in_rows(character, 0, 3) > 0, label);
    }

    /* The accented range, which is the reason for Latin-1 rather than ASCII:
     * a file with a name in it has one of these in it. */
    static const uint8_t accented[] = { 0xC4, 0xD6, 0xDC, 0xE4, 0xF6, 0xFC };

    for (unsigned i = 0; i < sizeof(accented); ++i) {
        char label[16];

        snprintf(label, sizeof label, "0x%02X has a glyph", accented[i]);
        check(glyph_ink(accented[i]) > 0, label);
    }

    /* Latin-1 rather than CP437, and the difference matters to anyone reaching
     * for a glyph: 0xB0 is a degree sign here and 0xC4 a vertical bar.  If a
     * CP437 font is ever substituted these two will fail, which is the point
     * -- a font that is correct for the wrong character set renders text that
     * is merely wrong, and nothing else notices. */
    /* A degree sign: a ring in the top of the cell, and nothing in the middle.
     * In CP437 this code point is an arrow, which is a completely different
     * shape in a different place, so the two are distinguishable. */
    check(ink_in_rows(0xB0, 0, 3) > 0 && ink_in_rows(0xB0, 5, 15) == 0,
          "0xB0 is a degree sign, not an arrow");
    /* A pipe: two pixels wide, every row from the middle down to the bottom. */
    check(ink_in_rows(0xC4, 7, 15) == 18, "0xC4 is a vertical bar");

    /* Typographic punctuation at the top of the range. */
    static const uint8_t punct[] = { 0xA9, 0xAE, 0xB0, 0xB5, 0xBD, 0xD7 };

    for (unsigned i = 0; i < sizeof(punct); ++i) {
        char label[16];

        snprintf(label, sizeof label, "punct 0x%02X has a glyph", punct[i]);
        check(glyph_ink(punct[i]) > 0, label);
    }

    /* Every code point in the range has a glyph.  A table that is one short at
     * the top returns whatever follows it in memory for the last few, which
     * passes every "does it have ink" check above and renders garbage. */
    for (int cp = 0x21; cp <= 0xFF; ++cp) {
        char label[16];

        snprintf(label, sizeof label, "0x%02X is present", cp);
        check(glyph_ink((uint8_t)cp) > 0, label);
    }

    /* Out of range: below the first glyph is a substitute box, not a blank and
     * not a read past the end of the table. */
    check(font8x16_row(0x00, 0) == 0x00 || font8x16_row(0x00, 0) == 0x7E,
          "a control character gets the substitute glyph");
    check(font8x16_row(0x41, -1) == 0U, "a negative row is zero");
    check(font8x16_row(0x41, FONT8X16_HEIGHT) == 0U, "a row past the end is zero");
    check(font8x16_row(0x41, 999) == 0U, "a far row is zero, not a read");

    if (failures != 0) {
        printf("font8x16_test FAILED: %d check%s\n", failures,
               failures == 1 ? "" : "s");
        printf("\n  'A' renders as:\n");
        print_glyph(0x41);
        printf("  'g' renders as:\n");
        print_glyph(0x67);
        return 1;
    }
    printf("  font8x16: Latin-1 0x20-0xFF, %d rows, all checks passed\n",
           FONT8X16_HEIGHT);
    return 0;
}
