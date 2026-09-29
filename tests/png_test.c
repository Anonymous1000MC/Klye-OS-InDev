/* PNG decoding, checked against a known good decode of a real image.
 *
 * This exists because the decoder was wrong in a way nothing else caught.  The
 * DEFLATE back reference window is 32 KiB, and the literal path only recorded a
 * byte in the window while there was room in it.  Past that point every literal
 * was still handed to the caller and then dropped, so the window stopped being
 * the recent output and the next back reference copied from a stale offset.
 *
 * The failure had no shape that a caller could notice: the stream emitted
 * exactly as many bytes as the file said it should, every length and distance
 * was correct, and png_decode() returned true.  Only the pixels were wrong, and
 * only below the row where the window happened to fill.  An image under 32 KiB
 * decoded perfectly, which is why the small generated fixtures all passed.
 *
 * So the test uses an image whose decoded output is comfortably larger than the
 * window -- 320x200 expands to 256,000 bytes, eight times the window -- and
 * compares the whole scaled surface against a CRC of a decode made with zlib.
 * A whole-surface CRC rather than a few probe pixels because the corruption is
 * positional: it starts at a row determined by the compression, so a spot check
 * near the top of the image passes on a broken decoder. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "png.h"

#define SURFACE_W 1280
#define SURFACE_H 720

/* png.c blends translucent pixels onto whatever the surface already holds, so
 * the host build needs the one function from gfx.c that it calls.  The kernel
 * has the real one; here it is the same arithmetic. */
uint32_t gfx_blend(uint32_t background, uint32_t foreground, uint32_t alpha)
{
    uint32_t red = ((foreground >> 16 & 0xFFU) * alpha +
                    (background >> 16 & 0xFFU) * (255U - alpha)) / 255U;
    uint32_t green = ((foreground >> 8 & 0xFFU) * alpha +
                      (background >> 8 & 0xFFU) * (255U - alpha)) / 255U;
    uint32_t blue = ((foreground & 0xFFU) * alpha +
                     (background & 0xFFU) * (255U - alpha)) / 255U;

    return (red << 16) | (green << 8) | blue;
}

/* CRC of the surface produced by decoding this file with zlib and scaling it
 * with floor(x * width / 1280) sampling, which is what the scaler does. */
#define EXPECTED_CRC 0x6BF5C0B4U

/* x=5 is clear of the wallpaper's glow; these are the same five rows the CRC
 * covers, kept as readable assertions so a failure says which way it went
 * wrong.  The row at 719 is the one that matters: it is the bottom of the
 * screen, and it was black on a decoder that had dropped its window. */
static const struct {
    int y;
    uint32_t rgb;
} probes[] = {
    { 0, 0x141E5AU },
    { 147, 0x142C5AU },
    { 200, 0x14315AU },
    { 400, 0x14445AU },
    { 719, 0x14635AU },
};

/* The same CRC, fed one pixel at a time in red, green, blue order.
 *
 * The surface stores 0x00RRGGBB, so hashing its memory directly would sum the
 * bytes in the machine's order -- blue, green, red, zero on x86 -- and the
 * expected value would depend on the endianness of whatever produced it.  Going
 * through the channels keeps the number meaning "these pixels". */
static uint32_t crc32_of_surface(const uint32_t *pixels, size_t count)
{
    uint32_t crc = 0xFFFFFFFFU;
    size_t at;

    for (at = 0; at < count; ++at) {
        uint32_t pixel = pixels[at] & 0x00FFFFFFU;
        int channel;

        for (channel = 16; channel >= 0; channel -= 8) {
            unsigned bit;

            crc ^= (pixel >> channel) & 0xFFU;
            for (bit = 0; bit < 8U; ++bit) {
                uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));

                crc = (crc >> 1) ^ (0xEDB88320U & mask);
            }
        }
    }
    return ~crc;
}

int main(int argc, char **argv)
{
    static uint8_t file[1U << 20];
    static uint32_t *surface;
    struct gfx_surface target;
    FILE *handle;
    size_t length;
    uint32_t crc;
    bool ok;
    int failures = 0;
    size_t index;

    if (argc < 2) {
        fprintf(stderr, "usage: png_test <file.png>\n");
        return 2;
    }
    handle = fopen(argv[1], "rb");
    if (handle == NULL) {
        fprintf(stderr, "png_test: cannot open %s\n", argv[1]);
        return 2;
    }
    length = fread(file, 1, sizeof(file), handle);
    fclose(handle);

    surface = calloc((size_t)SURFACE_W * SURFACE_H, 4U);
    if (surface == NULL) {
        fprintf(stderr, "png_test: out of memory\n");
        return 2;
    }
    target.pixels = surface;
    target.width = SURFACE_W;
    target.height = SURFACE_H;
    target.pitch_pixels = SURFACE_W;
    target.origin_x = 0;
    target.origin_y = 0;

    ok = png_decode(file, length, &target);
    if (!ok) {
        fprintf(stderr, "png_test FAILED: decode said no: %s\n", png_error());
        free(surface);
        return 1;
    }

    /* The rows are far apart on purpose.  If the window is dropped, the top of
     * the image is still right and only the later rows go black, so a probe at
     * the top alone would pass. */
    for (index = 0; index < sizeof(probes) / sizeof(probes[0]); ++index) {
        uint32_t got = surface[(size_t)probes[index].y * SURFACE_W + 5U];

        if ((got & 0x00FFFFFFU) != probes[index].rgb) {
            fprintf(stderr, "png_test FAILED: row %d is %06x, expected %06x\n",
                    probes[index].y, got & 0x00FFFFFFU, probes[index].rgb);
            failures++;
        }
    }

    crc = crc32_of_surface(surface, (size_t)SURFACE_W * SURFACE_H);
    if (crc != EXPECTED_CRC) {
        fprintf(stderr, "png_test FAILED: surface crc %08x, expected %08x\n",
                crc, EXPECTED_CRC);
        failures++;
    }

    free(surface);
    if (failures != 0) {
        return 1;
    }
    printf("png_test ok\n");
    return 0;
}
