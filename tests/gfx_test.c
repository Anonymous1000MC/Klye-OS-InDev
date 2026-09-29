/* Blending, checked at the two ends where an inverted argument shows up.
 *
 * gfx_blend's third argument is how much of the foreground to show.  It used to
 * be the other way round: the background was weighted by it and the foreground
 * by what was left, so alpha 255 returned the background and alpha 0 returned
 * the foreground.  The name says opacity, every caller assumed it, and PNG
 * stores its alpha channel the same way, so a fully transparent pixel came out
 * fully opaque and a caller asking for "mostly this colour" got mostly the old
 * one.
 *
 * That is invisible in a screenshot unless you are looking for it -- a bar drawn
 * at 232 came out as 9% bar and 91% whatever was behind it, which reads as "the
 * translucency is not doing much" rather than as a sign error.  So the two ends
 * are pinned here, where a mistake cannot hide. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gfx.h"
#include "theme.h"

#define BLACK PIXEL_RGB(0x00, 0x00, 0x00)
#define WHITE PIXEL_RGB(0xFF, 0xFF, 0xFF)
#define MID PIXEL_RGB(0x80, 0x80, 0x80)

/* Only gfx_init asks for these, and this test drives the blending functions
 * directly against a surface it set up itself, so they are here to satisfy the
 * linker rather than to be called. */
void *heap_alloc_pages(size_t bytes)
{
    (void)bytes;
    return NULL;
}

/* gfx.c panics rather than returning when the arena cannot be had.  Nothing in
 * this test asks for one, so a stub that fails loudly is enough. */
static int failures;

void panic(const char *message)
{
    fprintf(stderr, "gfx_test: unexpected panic: %s\n", message);
    failures++;
}

static void expect(uint32_t got, uint32_t want, const char *what)
{
    if ((got & 0x00FFFFFFU) != (want & 0x00FFFFFFU)) {
        fprintf(stderr, "gfx_test FAILED: %s: got %06x, expected %06x\n", what,
                got & 0x00FFFFFFU, want & 0x00FFFFFFU);
        failures++;
    }
}

static void test_blend_ends(void)
{
    expect(gfx_blend(BLACK, WHITE, 255U), WHITE, "alpha 255 is the foreground");
    expect(gfx_blend(BLACK, WHITE, 0U), BLACK, "alpha 0 is the background");
    expect(gfx_blend(WHITE, BLACK, 0U), WHITE,
           "alpha 0 leaves the background alone whichever way round it is");
    expect(gfx_blend(WHITE, BLACK, 255U), BLACK, "alpha 255 replaces it");
    /* halfway between black and white, and halfway between black and a mid
     * grey, which is not the same as the mid grey itself */
    expect(gfx_blend(BLACK, WHITE, 128U), PIXEL_RGB(0x80, 0x80, 0x80),
           "alpha 128 halfway to white");
    expect(gfx_blend(BLACK, MID, 128U), PIXEL_RGB(0x40, 0x40, 0x40),
           "alpha 128 halfway to mid grey");
    /* and the argument has to be monotonic, which is what an inversion breaks
     * in a way that only shows up partway along */
    if (!(gfx_blend(BLACK, WHITE, 64U) < gfx_blend(BLACK, WHITE, 192U))) {
        fprintf(stderr, "gfx_test FAILED: more alpha did not mean more "
                        "foreground\n");
        failures++;
    }
}

static void test_blend_rect(void)
{
    static uint32_t pixels[16 * 16];
    struct gfx_surface surface;
    int x;
    int y;

    surface.pixels = pixels;
    surface.width = 16;
    surface.height = 16;
    surface.pitch_pixels = 16;
    surface.origin_x = 0;
    surface.origin_y = 0;

    for (y = 0; y < 16; ++y) {
        for (x = 0; x < 16; ++x) {
            pixels[y * 16 + x] = BLACK;
        }
    }
    gfx_blend_rect(&surface, 0, 0, 16, 16, WHITE, 255U);
    expect(pixels[8 * 16 + 8], WHITE, "blend_rect at 255 is opaque");

    for (y = 0; y < 16; ++y) {
        for (x = 0; x < 16; ++x) {
            pixels[y * 16 + x] = BLACK;
        }
    }
    /* this is the one the menu bar got wrong: 232 asked to keep 232 parts of
     * the background, so the bar came out as 9% bar */
    gfx_blend_rect(&surface, 0, 0, 16, 16, WHITE, 232U);
    if (pixels[8 * 16 + 8] < 0xC0U) {
        fprintf(stderr, "gfx_test FAILED: blend_rect at 232 is %06x, which is "
                        "not mostly the foreground\n",
                pixels[8 * 16 + 8] & 0x00FFFFFFU);
        failures++;
    }
}

static void test_rounded_rect_alpha(void)
{
    static uint32_t pixels[32 * 32];
    struct gfx_surface surface;

    surface.pixels = pixels;
    surface.width = 32;
    surface.height = 32;
    surface.pitch_pixels = 32;
    surface.origin_x = 0;
    surface.origin_y = 0;

    for (int i = 0; i < 32 * 32; ++i) {
        pixels[i] = BLACK;
    }
    gfx_rounded_rect_alpha(&surface, 4, 4, 24, 24, 6, WHITE, 255U);
    /* the middle is well inside the radius, so the edge falloff does not reach
     * it and this is the fill colour and nothing else */
    expect(pixels[16 * 32 + 16], WHITE, "rounded_rect_alpha fills the middle");

    for (int i = 0; i < 32 * 32; ++i) {
        pixels[i] = BLACK;
    }
    gfx_rounded_rect_alpha(&surface, 4, 4, 24, 24, 6, WHITE, 0U);
    expect(pixels[16 * 32 + 16], BLACK, "rounded_rect_alpha at 0 draws nothing");
}

int main(void)
{
    test_blend_ends();
    test_blend_rect();
    test_rounded_rect_alpha();
    if (failures != 0) {
        return 1;
    }
    printf("gfx_test ok\n");
    return 0;
}
