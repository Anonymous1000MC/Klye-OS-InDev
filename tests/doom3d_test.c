/* doom3d_test.c - check the untextured 3D view against a real WAD.
 *
 * The renderer is worth trusting only if it draws a room from the real player
 * start of a real level.  So the test does not compare against a reference
 * image, which would only prove it has not changed; it checks the properties
 * that make a view correct: geometry is built from the level, a subsector's
 * sector is the one its segs say, and looking around from inside the level
 * actually fills the screen.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doom3d.h"
#include "doom_level.h"
#include "gfx.h"
#include "wad.h"

#define WIDTH 320
#define HEIGHT 200
#define BACKGROUND 0xFF101010U

static int failures;

static void check(int condition, const char *what)
{
    if (condition) {
        printf("  ok   %s\n", what);
    } else {
        printf("  FAIL %s\n", what);
        failures++;
    }
}

/* The first player 1 start, which is the viewpoint the level was designed to
 * be seen from.  Returns false when THINGS has no player start in it. */
static bool player_start(int16_t *x, int16_t *y)
{
    const struct wad_lump *marker = wad_find("E1M1");
    const struct wad_lump *thngs = 0;
    const struct wad_lump *linedef = 0;
    const struct wad_lump *sidedef = 0;
    const uint8_t *bytes;
    uint32_t count;
    uint32_t i;

    if (marker == 0 || !wad_level_parts(marker, &thngs, &linedef, &sidedef)) {
        return false;
    }
    bytes = wad_data(thngs);
    if (bytes == 0 || (thngs->size % 10U) != 0U) {
        return false;
    }
    count = thngs->size / 10U;
    for (i = 0; i < count; i++) {
        const uint8_t *t = bytes + i * 10U;
        int16_t type = (int16_t)((uint16_t)t[6] | ((uint16_t)t[7] << 8));

        if (type == 1) { /* player 1 start */
            *x = (int16_t)((uint16_t)t[0] | ((uint16_t)t[1] << 8));
            *y = (int16_t)((uint16_t)t[2] | ((uint16_t)t[3] << 8));
            return true;
        }
    }
    return false;
}

/* Share of the surface the renderer actually drew, in percent. */
static int coverage(uint32_t *pixels)
{
    struct gfx_surface surface;
    struct doom3d_stats stats;
    uint32_t filled = 0;
    uint32_t y;
    uint32_t x;

    surface.pixels = pixels;
    surface.width = WIDTH;
    surface.height = HEIGHT;
    surface.pitch_pixels = WIDTH;
    surface.origin_x = 0;
    surface.origin_y = 0;

    if (!doom3d_render(&surface, 0, 0, WIDTH, HEIGHT, BACKGROUND, &stats)) {
        printf("  render failed: %s\n", doom3d_error());
        failures++;
        return 0;
    }
    for (y = 0; y < HEIGHT; y++) {
        for (x = 0; x < WIDTH; x++) {
            if (pixels[y * WIDTH + x] != BACKGROUND) {
                filled++;
            }
        }
    }
    return (int)((filled * 100U) / ((uint32_t)WIDTH * HEIGHT));
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "DOOM1.wad";
    FILE *f;
    long size;
    uint8_t *data;
    uint32_t *pixels;
    int16_t start_x = 0;
    int16_t start_y = 0;
    int angle;
    int best = 0;
    int worst = 100;

    f = fopen(path, "rb");
    if (f == NULL) {
        printf("no WAD at %s\n", path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc((size_t)size);
    if (data == NULL || fread(data, 1, (size_t)size, f) != (size_t)size) {
        printf("could not read %s\n", path);
        fclose(f);
        return 1;
    }
    fclose(f);

    if (!wad_open(data, (uint32_t)size)) {
        printf("wad_open failed: %s\n", wad_error());
        return 1;
    }
    if (!doom_level_load("E1M1")) {
        printf("level load failed: %s\n", doom_level_error());
        return 1;
    }
    if (!doom3d_prepare()) {
        printf("prepare failed: %s\n", doom3d_error());
        return 1;
    }

    pixels = malloc((size_t)WIDTH * HEIGHT * sizeof(*pixels));
    if (pixels == NULL) {
        return 1;
    }

    printf("E1M1: %u subsectors, %u segs, %u sectors\n",
           doom_level()->subsector_count, doom_level()->seg_count,
           doom_level()->sector_count);
    printf("built %u segs, %u of them solid\n", doom3d_seg_count(),
           doom3d_solid_count());

    check(doom3d_seg_count() > 100, "the render list holds the level's segs");
    check(doom3d_solid_count() > 100, "most of the level is solid wall");
    check(doom3d_solid_count() < doom3d_seg_count(),
          "some segs are doorways, so rays can pass through");

    /* A subsector's sector must be the one its own segs report.  If the back
     * side case were taken from the right sidedef instead, rooms would be
     * shaded as their neighbour. */
    {
        int mismatch = 0;
        uint16_t i;

        for (i = 0; i < doom_level()->subsector_count; i++) {
            int via_helper = doom3d_subsector_sector(i);
            const struct doom_seg *seg;
            const struct doom_linedef *line;
            int side;
            int via_seg;

            if (doom_level()->subsectors[i].count == 0U) {
                continue;
            }
            /* recompute the same seg the helper uses, independently */
            seg = &doom_level()->segs[doom_level()->subsectors[i].first];
            line = &doom_level()->linedefs[seg->linedef];
            side = (seg->side == 0) ? line->right : line->left;
            via_seg = doom_level()->sidedefs[side].sector;
            if (via_helper != via_seg) {
                mismatch++;
            }
        }
        check(mismatch == 0, "subsector sectors agree with the segs");
    }

    if (!player_start(&start_x, &start_y)) {
        printf("no player start in THINGS\n");
        return 1;
    }
    printf("player start at (%d, %d)\n", start_x, start_y);

    /* Look all the way round from the player start.  A correct view fills
     * most of the screen in every direction, because the floor and ceiling of
     * the room alone cover every row. */
    for (angle = 0; angle < 360; angle += 45) {
        int got;

        doom3d_set_view(start_x, start_y, angle, 0);
        got = coverage(pixels);
        printf("  angle %3d deg -> %3d%% covered\n", angle, got);
        if (got < worst) {
            worst = got;
        }
        if (got > best) {
            best = got;
        }
    }
    check(worst > 55, "every direction from the player start fills the screen");
    check(best > 90, "some direction is essentially solid");

    /* And the view has to change as the player turns, or it is a flat fill
     * that happens to pass the coverage check. */
    {
        uint32_t *first = malloc((size_t)WIDTH * HEIGHT * sizeof(*first));
        uint32_t *second = malloc((size_t)WIDTH * HEIGHT * sizeof(*second));
        int differing = 0;

        doom3d_set_view(start_x, start_y, 0, 0);
        coverage(first);
        doom3d_set_view(start_x, start_y, 90, 0);
        coverage(second);
        for (angle = 0; angle < WIDTH * HEIGHT; angle++) {
            if (first[angle] != second[angle]) {
                differing++;
            }
        }
        check(differing > (WIDTH * HEIGHT) / 10,
              "turning 90 degrees changes the view");
        free(first);
        free(second);
    }

    /* The view has to land inside the rectangle it was given and nowhere else,
     * because a window is a rectangle inside a desktop rather than a surface of
     * its own.  Rendering into the full surface here would be fine and would
     * still pass every other check here. */
    {
        struct gfx_surface window;
        struct doom3d_stats stats;
        int inside_changed = 0;
        int outside_changed = 0;
        int i;

        for (i = 0; i < WIDTH * HEIGHT; i++) {
            pixels[i] = BACKGROUND;
        }
        window.pixels = pixels;
        window.width = WIDTH;
        window.height = HEIGHT;
        window.pitch_pixels = WIDTH;
        window.origin_x = 0;
        window.origin_y = 0;
        doom3d_set_view(start_x, start_y, 0, 0);
        if (!doom3d_render(&window, 40, 30, 160, 100, BACKGROUND, &stats)) {
            check(0, "rendering into a sub rectangle works");
        } else {
            check(1, "rendering into a sub rectangle works");
        }
        for (i = 0; i < WIDTH * HEIGHT; i++) {
            int x = i % WIDTH;
            int y = i / WIDTH;

            if (pixels[i] == BACKGROUND) {
                continue;
            }
            if (x >= 40 && x < 200 && y >= 30 && y < 130) {
                inside_changed++;
            } else {
                if (outside_changed < 6) {
                    printf("       outside at (%d,%d) = %08x\n", x, y, pixels[i]);
                }
                outside_changed++;
            }
        }
        check(inside_changed > 1000, "the sub rectangle was filled");
        check(outside_changed == 0, "nothing outside the sub rectangle changed");
    }

    free(pixels);
    free(data);

    if (failures != 0) {
        printf("doom3d_test FAILED (%d)\n", failures);
        return 1;
    }
    printf("doom3d_test ok\n");
    return 0;
}
