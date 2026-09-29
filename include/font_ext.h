#ifndef KLYE_FONT_EXT_H
#define KLYE_FONT_EXT_H

#include <stdbool.h>
#include <stdint.h>

#include "font.h"

/* Glyphs beyond printable ASCII, and the UTF-8 decoding that reaches them.
 *
 * The box drawing characters are three bytes each in UTF-8, so anything drawn
 * with them -- a logo, a table, a bar -- was invisible: the font covers 32 to
 * 126 and every other byte draws nothing.  Worse, a byte at a time meant each
 * character advanced three cells, so the text beside it drifted right.
 *
 * The decode lives here rather than in the font so the terminal and the font
 * cannot disagree about how many cells a string takes.  If they did, the
 * cursor and the glyphs would be counting differently and the whole line
 * would be wrong in a way that is very hard to see. */
#define FONT_EXT_GLYPH_COUNT 20U

/* One row of a glyph, for the given codepoint.  True when the font has
 * something for it, false when it does not, which is how a caller tells a
 * character it should leave alone from one it should draw a box for. */
bool font_ext_glyph(uint32_t codepoint, int row, uint8_t *bits);

/* Whether this codepoint has a glyph here, without fetching a row. */
bool font_ext_has(uint32_t codepoint);

/* The index of a codepoint in the table above, or -1 when it has none.
 *
 * A terminal cell holds one byte, and a box drawing character is three, so
 * the terminal stores the index rather than the sequence.  0xF0 plus the index
 * is the byte it stores: in the private use area, which no ASCII text can
 * produce, so it cannot be mistaken for a letter. */
int font_ext_index(uint32_t codepoint);

/* The byte the terminal stores in a cell for a box glyph, and the reverse.
 *
 * A cell is one byte and a box drawing character is three, so the cell holds
 * 0xF0 plus the index instead.  0xF0 is in the private use area, which no
 * ASCII text can produce, so a cell holding one is unambiguous. */
#define FONT_EXT_CELL_BASE 0xF0

/* The glyph row for a cell byte that holds a box glyph, false otherwise. */
bool font_ext_glyph_for_cell(uint8_t cell, int row, uint8_t *bits);

/* The next codepoint from a NUL terminated string, advancing *index past it.
 *
 * One codepoint per call, and the index is an in parameter so the caller keeps
 * its own position, which is what lets a caller count cells the same way the
 * glyphs are placed.  An invalid or truncated sequence yields U+FFFD and
 * consumes one byte, so a decoding mistake cannot make the rest of the string
 * unreadable. */
uint32_t font_utf8_next(const char *text, int *index);

#endif
