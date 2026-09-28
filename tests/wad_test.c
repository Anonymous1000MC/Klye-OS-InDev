/* Checks the WAD directory reader against a real WAD, and the name comparison
 * that everything else depends on.
 *
 * Pass the path to a WAD as argv[1].  Without one, the name comparison is still
 * checked on its own, which is the part that has bitten twice. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wad.h"

static int failures;

static void check(int condition, const char *what)
{
    if (!condition) {
        printf("FAIL %s\n", what);
        ++failures;
    } else {
        printf("ok   %s\n", what);
    }
}

/* Names are eight characters, NUL padded, and compared without regard to case
 * or padding.  The two that matter most here are a short name against a padded
 * one, and a name that does not exist against an entry with a blank name: the
 * latter used to match everything. */
static void name_tests(void)
{
    check(wad_name_is("SEGS    ", "SEGS") != 0, "padded name matches its short form");
    check(wad_name_is("SEGSEGV ", "SEGS") == 0, "a longer name does not match");
    check(wad_name_is("segs    ", "SEGS") != 0, "case is ignored");
    check(wad_name_is("        ", "SEGS") == 0, "a blank name matches nothing");
    check(wad_name_is("        ", "  ") != 0, "a blank name matches another blank");
    check(wad_name_is("PLAYPAL ", "PLAYPAL") != 0, "exact name matches");
    check(wad_name_is("PLAYPAL ", "TEXTURE1") == 0, "a different name does not");
}

int main(int argc, char **argv)
{
    name_tests();

    if (argc > 1) {
        FILE *file = fopen(argv[1], "rb");
        unsigned char *data;
        long size;
        const struct wad_lump *lump;
        int levels = 0;

        if (file == 0) {
            printf("cannot open %s\n", argv[1]);
            return 1;
        }
        fseek(file, 0, SEEK_END);
        size = ftell(file);
        fseek(file, 0, SEEK_SET);
        data = malloc((size_t)size);
        if (data == 0 || fread(data, 1, (size_t)size, file) != (size_t)size) {
            printf("cannot read %s\n", argv[1]);
            return 1;
        }
        fclose(file);

        if (wad_open(data, (uint32_t)size) == 0) {
            printf("wad_open failed: %s\n", wad_error());
            return 1;
        }
        check(strcmp(wad_kind(), "IWAD") == 0 || strcmp(wad_kind(), "PWAD") == 0,
              "magic is IWAD or PWAD");
        printf("     %d lumps, %ld bytes\n", wad_count(), size);

        check(wad_find("PLAYPAL") != 0, "PLAYPAL is found");
        check(wad_find("COLORMAP") != 0, "COLORMAP is found");
        check(wad_find("ZZZZZZZZ") == 0, "a name that does not exist is not found");
        check(wad_find("") == 0, "an empty name is not found");

        lump = wad_find("PLAYPAL");
        if (lump != 0) {
            check(lump->size == 10752, "PLAYPAL is 10752 bytes");
            check(lump->loaded != 0, "PLAYPAL resolves to data");
        }
        lump = wad_find("E1M1");
        check(lump != 0, "E1M1 is found");
        if (lump != 0) {
            check(lump->size == 0, "the E1M1 marker carries no data");
        }

        for (int index = 0; index < wad_count(); ++index) {
            if (wad_is_level_marker(wad_at(index))) {
                ++levels;
            }
        }
        check(levels == 9, "there are 9 level markers");

        /* ANIMATED is what separates a 1.9 WAD from the older shareware one,
         * where a sector's floor and ceiling are texture names rather than
         * PNAMES indices.  DOOM1.WAD is the older layout. */
        printf("     version 1.9: %s\n", wad_is_version_199() ? "yes" : "no");
    }

    if (failures != 0) {
        printf("%d wad check(s) failed\n", failures);
        return 1;
    }
    printf("all wad checks passed\n");
    return 0;
}
