/* A host-side preview of bilinear glyph scaling.
 *
 * The kernel's glyph_draw takes an integer scale and paints each source pixel
 * as a scale x scale block of destination pixels.  That is nearest-neighbour,
 * and it is why text looks like blocks: a stem that is one source pixel wide
 * becomes a staircase at any scale above one, and at 8x the original bitmap is
 * visible as 8x8 tiles.
 *
 * Bilinear treats the glyph as a coverage image instead.  Each destination
 * pixel samples four source pixels weighted by how far it sits between them,
 * and the result is an alpha value between 0 and 255 rather than a bit.  The
 * text is then blended into the destination by that coverage, which is what
 * gives a stem a soft edge that follows the glyph's shape rather than a stair
 * that follows the pixel grid.
 *
 * This program renders the same text both ways so the difference can be seen
 * side by side without booting anything.
 *
 * Built and run on the host; it is a preview, not a kernel component. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

/* An 8x16 face, enough to see the effect.  Taken from the console font the
 * kernel now carries, so what is shown here is what the kernel would draw. */
static const uint8_t glyph_A[16] = {
    0x00, 0x00, 0x10, 0x38, 0x6C, 0xC6, 0xC6, 0xFE,
    0xC6, 0xC6, 0xC6, 0xC6, 0x00, 0x00, 0x00, 0x00
};
static const uint8_t glyph_g[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x76, 0xCC, 0xCC,
    0xCC, 0xCC, 0xCC, 0x76, 0x0C, 0xCC, 0x78, 0x00
};
static const uint8_t glyph_e[16] = {
    0x00, 0x00, 0x00, 0x00, 0x7C, 0xC6, 0xC0, 0x7C,
    0x06, 0xC6, 0xC6, 0x7C, 0x00, 0x00, 0x00, 0x00
};
static const uint8_t glyph_K[16] = {
    0x00, 0x00, 0xC6, 0xCC, 0xD8, 0xF0, 0xD8, 0xCC,
    0xC6, 0xC6, 0xC6, 0xC6, 0x00, 0x00, 0x00, 0x00
};
static const uint8_t glyph_space[16] = { 0 };
static const uint8_t glyph_y[16] = {
    0x00, 0x00, 0x00, 0x00, 0xC6, 0xC6, 0xC6, 0xC6,
    0xC6, 0x7E, 0x06, 0x0C, 0xF8, 0x00, 0x00, 0x00
};

#define GW 8
#define GH 16

static const uint8_t *lookup(char c)
{
    switch (c) {
    case 'A': return glyph_A;
    case 'g': return glyph_g;
    case 'e': return glyph_e;
    case 'K': return glyph_K;
    case 'y': return glyph_y;
    default:  return glyph_space;
    }
}

/* The bit for a source column.  The bitmap stores bit 0 as the leftmost
 * pixel, which is what the kernel's glyph_draw assumes. */
static int source_bit(const uint8_t *glyph, int row, int column)
{
    if (row < 0 || row >= GH || column < 0 || column >= GW) {
        return 0;
    }
    return (glyph[row] >> column) & 1;
}

/* Nearest-neighbour, exactly as the kernel does it: pick one source row and
 * one source column per destination row and column, then paint a block. */
static unsigned nearest(const uint8_t *glyph, int dx, int dy, int scale)
{
    int sr = dy / scale;
    int sc = dx / scale;

    return source_bit(glyph, sr, sc) ? 255U : 0U;
}

/* Bilinear.  The destination pixel's centre is mapped back into source space,
 * and the four neighbours around it are weighted by how far it sits between
 * them.  Coverage comes out as a number rather than a bit. */
static unsigned bilinear(const uint8_t *glyph, double dx, double dy,
                         int dest_h)
{
    /* Destination pixel centre -> source pixel centre.
     *
     * The divisor is the destination extent, so at 2x the glyph's 16 rows map
     * across 32 destination rows.  Using the source height here instead made
     * the sample walk off the bottom of the glyph, which ate the descenders:
     * the 'g' lost its tail and the 'y' its descender, and every letter sat
     * high in its cell.  It is exactly the kind of error that reads as "the
     * preview looks wrong" rather than as a maths mistake. */
    double sx = (dx + 0.5) * GW / (double)(GW) - 0.5;
    double sy = (dy + 0.5) * GH / (double)dest_h - 0.5;

    int x0 = (int)floor(sx);
    int y0 = (int)floor(sy);
    double fx = sx - x0;
    double fy = sy - y0;

    double w00 = (1.0 - fx) * (1.0 - fy);
    double w10 = fx * (1.0 - fy);
    double w01 = (1.0 - fx) * fy;
    double w11 = fx * fy;

    double v = w00 * source_bit(glyph, y0, x0) +
               w10 * source_bit(glyph, y0, x0 + 1) +
               w01 * source_bit(glyph, y0 + 1, x0) +
               w11 * source_bit(glyph, y0 + 1, x0 + 1);

    unsigned a = (unsigned)(v * 255.0 + 0.5);

    return a > 255U ? 255U : a;
}

static void render(const char *label, const char *text, int scale, int use_bilinear)
{
    int width = (int)strlen(text) * GW * scale;
    int height = GH * scale;
    unsigned char *canvas = calloc((size_t)width * (size_t)height, 1);

    for (int cx = 0; cx < width; ++cx) {
        for (int cy = 0; cy < height; ++cy) {
            int cell = cx / (GW * scale);
            int dx = cx % (GW * scale);
            int dy = cy;
            const uint8_t *glyph = lookup(text[cell]);
            unsigned a;

            if (use_bilinear) {
                a = bilinear(glyph, dx, dy, height);
            } else {
                a = nearest(glyph, dx, dy, scale);
            }
            canvas[(size_t)cy * (size_t)width + (size_t)cx] = (unsigned char)a;
        }
    }

    printf("%s   (%dx%d, %s)\n", label, width, height,
           use_bilinear ? "bilinear" : "nearest, what it does now");
    for (int cy = 0; cy < height; ++cy) {
        fputs("  ", stdout);
        for (int cx = 0; cx < width; ++cx) {
            unsigned char a = canvas[(size_t)cy * (size_t)width + (size_t)cx];

            /* Printed as intensity so the soft edge is visible as greys. */
            const char *ramp = " .:-=+*#%@";

            putchar(ramp[a * 9U / 255U]);
        }
        putchar('\n');
    }
    putchar('\n');
    free(canvas);
}

int main(int argc, char **argv)
{
    int scale = (argc > 1) ? atoi(argv[1]) : 2;

    if (scale < 1 || scale > 4) {
        scale = 2;
    }
    printf("8x16 glyphs at %dx, printing coverage as greys\n\n", scale);
    render("BEFORE (nearest)", "Age", scale, 0);
    render("AFTER  (bilinear)", "Age", scale, 1);
    render("BEFORE (nearest)", "Klye", scale, 0);
    render("AFTER  (bilinear)", "Klye", scale, 1);
    return 0;
}
