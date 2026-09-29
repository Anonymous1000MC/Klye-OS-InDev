#ifndef KLYE_PNG_H
#define KLYE_PNG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gfx.h"

/* Decode a PNG and scale it over a surface.
 *
 * Built on inflate.c: a PNG's image data is a zlib stream around DEFLATE, and
 * there was no DEFLATE anywhere in this tree.
 *
 * Supported: 8 and 16 bits per channel, greyscale, greyscale+alpha, truecolour,
 * truecolour+alpha, and palette images, with tRNS transparency, at any size.
 * Refused by name, rather than drawn wrong: interlaced images, and bit depths
 * and colour types outside the above.
 *
 * The image is scaled to cover the surface, sampled by area rather than by
 * nearest pixel, because a nearest neighbour stretch of anything smaller than
 * the screen is a mosaic.
 *
 * Returns false with a reason in png_error(). */
bool png_decode(const uint8_t *data, size_t length,
                struct gfx_surface *surface);

const char *png_error(void);

#endif
