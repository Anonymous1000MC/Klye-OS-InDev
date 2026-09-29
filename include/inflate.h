#ifndef KLYE_INFLATE_H
#define KLYE_INFLATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* DEFLATE decompression, for PNG.
 *
 * A PNG's image data is a zlib stream around DEFLATE, and there is no
 * implementation of DEFLATE in this tree, so this is one.  It covers all three
 * block types, which is the whole of the format an encoder may emit.
 *
 * Output goes through a callback one byte at a time rather than into a buffer.
 * A PNG row cannot be defiltered until the row above it has been filtered, so
 * the consumer has to see the bytes in order; handing it a whole match at once
 * would hand it bytes it is not ready for.
 *
 * `history` is the back reference window and is also where the bytes already
 * emitted are kept, because a length/distance pair refers back into the output
 * rather than into the input.  It must be at least 32 KiB, which is the largest
 * distance DEFLATE can express.
 *
 * Returns false on a corrupt or truncated stream, with a reason in
 * inflate_error(). */
#define INFLATE_MAX_SYMBOLS 288

typedef bool (*inflate_emit_fn)(void *context, uint8_t byte);

bool inflate_stream(const uint8_t *input, size_t input_length,
                    size_t expected_output, uint8_t *history,
                    size_t history_length, inflate_emit_fn emit, void *context);

const char *inflate_error(void);

#endif
