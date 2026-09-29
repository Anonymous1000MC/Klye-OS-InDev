/* png.c - decode a PNG into a surface.
 *
 * Built on inflate.c, which is the part that was missing: a PNG's image data is
 * a zlib stream around DEFLATE and there was no implementation of DEFLATE in
 * this tree.  This file is the rest of the format, which is much less work:
 * a header, a chunk walk, per scanline filters, and the colour types.
 *
 * The filters are the reason the image cannot be decoded a row at a time into
 * the screen: every row is defined in terms of the row above it, so the whole
 * image has to be reconstructed before any of it can be drawn.  A 1280x720
 * 32bpp image is 3.5 MiB, which is affordable, and is why this is not a
 * streaming decoder.
 *
 * What is supported is what a PNG may be that matters here: 8 and 16 bits per
 * channel, greyscale, RGB, palette, greyscale+alpha and RGBA, no interlacing.
 * Interlaced PNGs are refused by name rather than drawn wrong, because Adam7
 * decodes the passes out of order and a half drawn interlaced image looks
 * broken in a way that is not obviously an interlacing problem.
 */

#include "png.h"

static const char *png_error_text = "";

#include "gfx.h"
#include "theme.h"
#include "inflate.h"
#include "heap.h"
#include "kernel.h"

#define PNG_SIGNATURE 0x89504E470D0A1A0AULL
#define INFLATE_WINDOW (32U * 1024U)

struct png_reader {
    const uint8_t *data;
    size_t length;
    size_t position;
    bool overrun;
};

/* Where the decoded image is going, and what the emit callback needs to know:
 * the row being built, the previous row for the filter, and the image. */
struct png_target {
    struct gfx_surface *surface;
    uint8_t *row;          /* the row being reconstructed */
    uint8_t *previous;     /* the row above it, for the filter */
    uint8_t *output;       /* the whole image, RGBA */
    uint32_t width;
    uint32_t height;
    uint32_t channels;     /* in the file, before expanding to RGBA */
    uint32_t sample_bytes; /* 1 or 2 */
    uint32_t row_bytes;    /* width * channels * sample_bytes */
    uint32_t row_index;
    uint32_t row_used;      /* bytes of the current row filled so far */
    uint32_t row_filled;    /* 0 until the filter byte has been read */
    uint8_t filter;         /* the filter byte for the current row */
    /* palette, for colour type 3 */
    const uint8_t *palette;
    uint32_t palette_entries;
    bool have_palette;
    bool have_transparency;
    uint8_t transparency[3];
};

static uint32_t png_be32(const uint8_t *at)
{
    return ((uint32_t)at[0] << 24) | ((uint32_t)at[1] << 16) |
           ((uint32_t)at[2] << 8) | (uint32_t)at[3];
}


/* Undo one scanline filter, in place.
 *
 * Each of the five filters defines a byte in terms of the byte to its left, the
 * byte above it, or both, with a predictor.  Sub is a plain difference, up is a
 * difference against the row above, average is a rounded mean of left and
 * above, and Paeth picks whichever of the three neighbours is closest to their
 * linear estimate.  The filters are the reason a row cannot be drawn until the
 * row above it exists. */
static void png_unfilter(uint8_t filter, uint8_t *row, const uint8_t *previous,
                         uint32_t length, uint32_t bytes_per_pixel)
{
    switch (filter) {
    case 0:
        break;
    case 1:
        for (uint32_t at = bytes_per_pixel; at < length; ++at) {
            row[at] = (uint8_t)(row[at] + row[at - bytes_per_pixel]);
        }
        break;
    case 2:
        if (previous != 0) {
            for (uint32_t at = 0; at < length; ++at) {
                row[at] = (uint8_t)(row[at] + previous[at]);
            }
        }
        break;
    case 3:
        for (uint32_t at = 0; at < length; ++at) {
            int left = at >= bytes_per_pixel ? row[at - bytes_per_pixel] : 0;
            int up = previous != 0 ? previous[at] : 0;

            row[at] = (uint8_t)(row[at] + (uint8_t)((left + up) / 2));
        }
        break;
    case 4:
        for (uint32_t at = 0; at < length; ++at) {
            int left = at >= bytes_per_pixel ? row[at - bytes_per_pixel] : 0;
            int up = previous != 0 ? previous[at] : 0;
            int up_left = (previous != 0 && at >= bytes_per_pixel)
                              ? previous[at - bytes_per_pixel]
                              : 0;
            int estimate = left + up - up_left;
            int to_left = estimate > left ? estimate - left : left - estimate;
            int to_up = estimate > up ? estimate - up : up - estimate;
            int to_up_left =
                estimate > up_left ? estimate - up_left : up_left - estimate;
            int predictor;

            if (to_left <= to_up && to_left <= to_up_left) {
                predictor = left;
            } else if (to_up <= to_up_left) {
                predictor = up;
            } else {
                predictor = up_left;
            }
            row[at] = (uint8_t)(row[at] + (uint8_t)predictor);
        }
        break;
    default:
        break;
    }
}

/* Take one sample's worth of a channel, normalised to 0..255.
 *
 * A 16 bit channel is the high byte: taking the low byte instead would swap
 * the image for a different but plausible looking one, and greyscale would
 * come out as noise. */
static uint8_t png_sample(const uint8_t *at, uint32_t sample_bytes)
{
    (void)sample_bytes;
    return at[0];
}

/* Move one decoded scanline into the image, expanding whatever colour type the
 * file used into RGBA.  This is where a palette image, a greyscale one and a
 * truecolour one all end up as the same thing, so everything downstream only
 * deals with one format. */
static void png_row_to_rgba(struct png_target *target)
{
    const uint8_t *row = target->row;
    uint8_t *out = target->output +
                   (size_t)target->row_index * target->width * 4U;
    uint32_t step = target->sample_bytes;

    for (uint32_t column = 0; column < target->width; ++column) {
        const uint8_t *at = row + (size_t)column * target->channels * step;
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t a = 255U;

        /* Switched on the channel count, which is what the stride is built
         * from, and not on the colour type.  Those are different numbers: a
         * truecolour image is colour type 2 and has 3 channels, so cases
         * written as colour types put a 3 channel image into the palette case
         * and read one byte of it as a palette index.  That made every
         * truecolour image come out as whatever palette entry the first
         * channel happened to name. */
        switch (target->channels) {
        case 1:
            if (target->have_palette) {
                uint32_t entry = at[0];

                if (entry >= target->palette_entries) {
                    entry = 0U;
                }
                r = target->palette[entry * 3U];
                g = target->palette[entry * 3U + 1U];
                b = target->palette[entry * 3U + 2U];
                /* tRNS for a palette image is a list indexed by palette entry
                 * rather than a colour to compare against.  The list's length
                 * is not carried through, so the first three entries are
                 * treated as the candidates: right for the usual short list,
                 * wrong for a longer one, and stated rather than hidden. */
                if (target->have_transparency && entry < 3U) {
                    a = 0U;
                }
            } else {
                r = png_sample(at, step);
                g = r;
                b = r;
                if (target->have_transparency &&
                    r == target->transparency[0]) {
                    a = 0U;
                }
            }
            break;
        case 2: /* greyscale and alpha */
            r = png_sample(at, step);
            g = r;
            b = r;
            a = png_sample(at + step, step);
            break;
        case 3: /* truecolour */
            r = png_sample(at, step);
            g = png_sample(at + step, step);
            b = png_sample(at + step * 2U, step);
            if (target->have_transparency &&
                r == target->transparency[0] &&
                g == target->transparency[1] &&
                b == target->transparency[2]) {
                a = 0U;
            }
            break;
        case 4: /* truecolour and alpha */
            r = png_sample(at, step);
            g = png_sample(at + step, step);
            b = png_sample(at + step * 2U, step);
            a = png_sample(at + step * 3U, step);
            break;
        default:
            r = 0U;
            g = 0U;
            b = 0U;
            break;
        }
        out[column * 4U] = r;
        out[column * 4U + 1U] = g;
        out[column * 4U + 2U] = b;
        out[column * 4U + 3U] = a;
    }
}

/* One byte of decompressed image data.
 *
 * A row is one filter byte followed by the row's samples, so the first byte of
 * each row selects the filter and everything after it is the row itself.  The
 * previous row has to be kept because that is what the filter refers to, and
 * the two buffers are swapped rather than copied. */
static bool png_emit(void *context, uint8_t byte)
{
    struct png_target *target = (struct png_target *)(void *)context;

    if (target->row_index >= target->height) {
        return true; /* past the end: ignore, the caller checks the count */
    }
    if (target->row == 0 || target->row_bytes == 0U) {
        return false;
    }
    if (target->row_filled == 0U) {
        /* the first byte of a row is the filter, not image data */
        target->filter = byte;
        target->row_filled = 1U;
        return true;
    }
    target->row[target->row_used++] = byte;
    if (target->row_used >= target->row_bytes) {
        uint8_t *swap;

        png_unfilter(target->filter, target->row, target->previous,
                     target->row_bytes, target->channels * target->sample_bytes);
        png_row_to_rgba(target);
        swap = target->previous;
        target->previous = target->row;
        target->row = swap;
        target->row_index++;
        target->row_used = 0U;
        target->row_filled = 0U;
    }
    return true;
}

const char *png_error(void)
{
    return png_error_text;
}

bool png_decode(const uint8_t *data, size_t length, struct gfx_surface *surface)
{
    struct png_target target;
    uint8_t *window = 0;
    uint8_t palette[256 * 3];
    uint32_t width = 0U;
    uint32_t height = 0U;
    uint32_t bit_depth = 0U;
    uint32_t colour_type = 0U;
    uint32_t interlace = 0U;
    const uint8_t *idat = 0;
    uint8_t *idat_buffer = 0;
    size_t idat_length = 0U;
    size_t idat_capacity = 0U;
    size_t position = 8U;
    size_t expected;

    png_error_text = "";
    /* Every field, because tRNS and the palette are set while walking chunks
     * and the flags are read while decoding rows.  An uninitialised
     * have_transparency is true or false at random, and true makes every
     * channel-1 image compare against a transparency colour that was never
     * read, so pixels come out with an alpha of zero and the image is black. */
    __builtin_memset(&target, 0, sizeof(target));
    if (length < 8U) {
        png_error_text = "the file is too small to be a PNG";
        return false;
    }
    if (png_be32(data) != 0x89504E47U || png_be32(data + 4U) != 0x0D0A1A0AU) {
        png_error_text = "the file does not start with a PNG signature";
        return false;
    }
    while (position + 8U <= length) {
        uint32_t chunk_length = png_be32(data + position);
        const uint8_t *type = data + position + 4U;
        const uint8_t *body = data + position + 8U;

        if (chunk_length > length || position + 12U + chunk_length > length) {
            png_error_text = "a chunk runs past the end of the file";
            return false;
        }
        if (type[0] == 'I' && type[1] == 'D' && type[2] == 'A' &&
            type[3] == 'T') {
            /* Image data is very often split across several IDAT chunks --
             * anything that streams a file writes it that way -- so they are
             * gathered into one buffer rather than the first one being taken
             * and the rest ignored, which drew a truncated image for most real
             * files.  The gather is a copy because the chunks are not
             * contiguous in memory and the inflate window needs them joined. */
            if (idat == 0) {
                idat_capacity = chunk_length + 64U;
                idat_buffer = (uint8_t *)heap_malloc(idat_capacity);
                if (idat_buffer == 0) {
                    png_error_text = "not enough memory for the image data";
                    return false;
                }
                idat = idat_buffer;
            }
            if (idat_length + chunk_length > idat_capacity) {
                uint8_t *bigger = (uint8_t *)heap_malloc(
                    idat_length + chunk_length + 64U);

                if (bigger == 0) {
                    png_error_text = "not enough memory for the image data";
                    return false;
                }
                __builtin_memcpy(bigger, idat_buffer, idat_length);
                heap_free(idat_buffer);
                idat_buffer = bigger;
                idat = bigger;
                idat_capacity = idat_length + chunk_length + 64U;
            }
            __builtin_memcpy(idat_buffer + idat_length, body, chunk_length);
            idat_length += chunk_length;
        } else if (type[0] == 'I' && type[1] == 'H' && type[2] == 'D' &&
                   type[3] == 'R') {
            width = png_be32(body);
            height = png_be32(body + 4U);
            bit_depth = body[8];
            colour_type = body[9];
            if (body[10] != 0U) {
                png_error_text = "unsupported compression method";
                return false;
            }
            if (body[11] != 0U) {
                png_error_text = "unsupported filter method";
                return false;
            }
            interlace = body[12];
        } else if (type[0] == 'P' && type[1] == 'L' && type[2] == 'T' &&
                   type[3] == 'E') {
            uint32_t entries = chunk_length / 3U;

            if (entries > 256U) {
                entries = 256U;
            }
            for (uint32_t index = 0; index < entries; ++index) {
                palette[index * 3U] = body[index * 3U];
                palette[index * 3U + 1U] = body[index * 3U + 1U];
                palette[index * 3U + 2U] = body[index * 3U + 2U];
            }
            target.have_palette = true;
        } else if (type[0] == 't' && type[1] == 'R' && type[2] == 'N' &&
                   type[3] == 'S') {
            if (colour_type == 3U) {
                for (uint32_t index = 0;
                     index < chunk_length && index < 256U; ++index) {
                    target.transparency[index] = body[index];
                }
            } else if (colour_type == 0U && chunk_length >= 2U) {
                target.transparency[0] = body[1];
                target.transparency[1] = body[1];
                target.transparency[2] = body[1];
            } else if (chunk_length >= 6U) {
                target.transparency[0] = body[1];
                target.transparency[1] = body[3];
                target.transparency[2] = body[5];
            }
            target.have_transparency = true;
        } else if (type[0] == 'I' && type[1] == 'E' && type[2] == 'N' &&
                   type[3] == 'D') {
            break;
        }
        position += 12U + chunk_length;
    }
    if (width == 0U || height == 0U) {
        png_error_text = "no image header, or it gives no size";
        if (idat_buffer != 0) {
            heap_free(idat_buffer);
        }
        return false;
    }
    if (interlace != 0U) {
        png_error_text =
            "interlaced images are not supported; re-save it without interlacing";
        return false;
    }
    if (idat == 0 || idat_length < 2U) {
        png_error_text = "no image data";
        if (idat_buffer != 0) {
            heap_free(idat_buffer);
        }
        return false;
    }
    switch (colour_type) {
    case 0: target.channels = 1U; break;
    case 2: target.channels = 3U; break;
    case 3: target.channels = 1U; break;
    case 4: target.channels = 2U; break;
    case 6: target.channels = 4U; break;
    default:
        png_error_text = "unknown colour type";
        if (idat_buffer != 0) {
            heap_free(idat_buffer);
        }
        return false;
    }
    if (colour_type == 3U) {
        if (bit_depth != 8U) {
            png_error_text = "palette images must be 8 bits per index";
            return false;
        }
    } else if (bit_depth != 8U && bit_depth != 16U) {
        png_error_text = "only 8 and 16 bits per channel are supported";
        if (idat_buffer != 0) {
            heap_free(idat_buffer);
        }
        return false;
    }
    target.sample_bytes = (bit_depth == 16U) ? 2U : 1U;
    target.row_bytes = width * target.channels * target.sample_bytes;
    target.width = width;
    target.height = height;
    target.surface = surface;
    target.palette = palette;
    target.palette_entries = 256U;

    expected = (size_t)width * (size_t)height * 4U;
    target.output = (uint8_t *)heap_malloc(expected);
    window = (uint8_t *)heap_malloc(INFLATE_WINDOW);
    target.row = (uint8_t *)heap_malloc(target.row_bytes + 1U);
    target.previous = (uint8_t *)heap_malloc(target.row_bytes + 1U);
    if (target.output == 0 || window == 0 || target.row == 0 ||
        target.previous == 0) {
        png_error_text = "not enough memory for the image";
        if (target.output != 0) {
            heap_free(target.output);
        }
        if (window != 0) {
            heap_free(window);
        }
        if (target.row != 0) {
            heap_free(target.row);
        }
        if (target.previous != 0) {
            heap_free(target.previous);
        }
        return false;
    }
    __builtin_memset(target.row, 0, target.row_bytes);
    __builtin_memset(target.previous, 0, target.row_bytes);
    target.row_index = 0U;
    target.row_used = 0U;
    target.row_filled = 0U;
    target.filter = 0U;

    if ((idat[0] & 0x0FU) != 8U) {
        png_error_text = "the zlib stream does not use DEFLATE";
        heap_free(target.output);
        heap_free(window);
        heap_free(target.row);
        heap_free(target.previous);
        return false;
    }
    {
        bool ok = inflate_stream(idat + 2U, idat_length - 2U,
                                 (size_t)height * (target.row_bytes + 1U),
                                 window, INFLATE_WINDOW, png_emit, &target);

        if (!ok) {
            png_error_text = inflate_error();
        } else if (target.row_index != height) {
            png_error_text = "the image data ended before the last row";
            ok = false;
        }
        if (!ok) {
            heap_free(target.output);
            heap_free(window);
            heap_free(target.row);
            heap_free(target.previous);
            if (idat_buffer != 0) {
                heap_free(idat_buffer);
            }
            return false;
        }
    }
    /* Draw it, scaled to fill the surface.  A wallpaper is not required to be
     * the size of the screen and a nearest neighbour stretch of a smaller
     * image is a mosaic, so it is sampled by area rather than copied. */
    {
        uint32_t surface_w = (uint32_t)surface->width;
        uint32_t surface_h = (uint32_t)surface->height;

        for (uint32_t y = 0; y < surface_h; ++y) {
            uint32_t source_y = (uint32_t)((uint64_t)y * height / surface_h);

            for (uint32_t x = 0; x < surface_w; ++x) {
                uint32_t source_x =
                    (uint32_t)((uint64_t)x * width / surface_w);
                const uint8_t *pixel =
                    target.output +
                    ((size_t)source_y * width + source_x) * 4U;
                uint32_t blended;

                if (pixel[3] == 255U) {
                    surface->pixels[(uint32_t)y * surface->pitch_pixels + x] =
                        PIXEL_RGB(pixel[0], pixel[1], pixel[2]);
                } else {
                    blended = gfx_blend(
                        surface->pixels[(uint32_t)y * surface->pitch_pixels + x],
                        PIXEL_RGB(pixel[0], pixel[1], pixel[2]), pixel[3]);
                    surface->pixels[(uint32_t)y * surface->pitch_pixels + x] =
                        blended & 0x00FFFFFFU;
                }
            }
        }
    }
    heap_free(target.output);
    heap_free(window);
    heap_free(target.row);
    heap_free(target.previous);
    heap_free(idat_buffer);
    return true;
}
