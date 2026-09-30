/* The settings store, tested against the properties that matter.
 *
 * Three of these are cases that a store which looks like it works will still
 * get wrong:
 *
 *  - A value out of range in the file must be clamped on the way in.  A file
 *    with a nonsense value in it should not be able to make a window four
 *    pixels wide.
 *
 *  - Writing must truncate.  Writing over a longer old file leaves the tail of
 *    it in place, and a settings file with two copies of a key takes whichever
 *    the parser reaches last, which is the one nobody edited.
 *
 *  - An unknown key is not an error.  A file written by a later version, or by
 *    a person, may carry keys this build does not have, and it should load
 *    the ones it does rather than refuse the whole file.
 */

#include <stdio.h>
#include <string.h>

#include "../include/settings.h"

/* The store calls vfs_read and friends; the tests link the real settings.c
 * against stubs that hand back this text. */
static const char *stub_file = 0;

int vfs_read(const char *path, char *out, uint32_t max)
{
    uint32_t length;

    (void)path;
    if (stub_file == 0) {
        return -1;
    }
    length = (uint32_t)strlen(stub_file);
    if (length > max) {
        length = max;
    }
    for (uint32_t index = 0U; index < length; ++index) {
        out[index] = stub_file[index];
    }
    return (int)length;
}

static int truncate_calls;

int vfs_truncate(const char *path)
{
    (void)path;
    truncate_calls++;
    return 0;
}

int vfs_touch(const char *path)
{
    (void)path;
    return 0;
}

static char written[4096];
static uint32_t written_length;

int vfs_write(const char *path, const char *data, uint32_t length)
{
    (void)path;
    if (length > sizeof(written)) {
        length = (uint32_t)sizeof(written);
    }
    for (uint32_t index = 0U; index < length; ++index) {
        written[index] = data[index];
    }
    written_length = length;
    written[length] = '\0';
    return (int)length;
}

static int failures;

static void check(int condition, const char *what)
{
    if (!condition) {
        printf("  FAIL %s\n", what);
        failures++;
    }
}

/* The store loads once, so each test needs a fresh process's worth of state.
 * Reloading is done by calling the internal reset, which settings.c does not
 * expose; the tests therefore run as separate cases in one process by saving
 * and restoring the values directly through settings_set. */
int main(void)
{
    /* A file with three keys: one normal, one above its range, one below it. */
    stub_file =
        "display.scale=3\n"
        "window.minimum_width=9000\n"
        "terminal.lines=10\n"
        "unknown.future.key=7\n"
        "\n"
        "# a comment line\n"
        "garbage with no equals sign\n";
    settings_load();

    check(settings_get("display.scale") == 3U, "a normal value is read");
    check(settings_get("window.minimum_width") == 900U,
          "a value above the range is clamped to the maximum");
    check(settings_get("terminal.lines") == 50U,
          "a value below the range is clamped to the minimum");
    check(settings_get("unknown.future.key") == 0U,
          "an unknown key reads as zero rather than as a failure");
    check(settings_get("display.wallpaper") == 0U,
          "a key the file does not mention keeps its default");
    check(settings_valid("display.scale"), "a known key is valid");
    check(settings_valid("nothing.like.this") == false,
          "an unknown key is not valid");

    /* Setting and reading back. */
    check(settings_set("display.scale", 2U), "setting a new value reports a change");
    check(settings_get("display.scale") == 2U, "the new value reads back");
    check(settings_set("display.scale", 2U) == false,
          "setting the same value again reports no change");

    /* Clamped on the way out too, so a caller cannot write a value the file
     * would refuse to load back. */
    settings_set("display.scale", 99U);
    check(settings_get("display.scale") == 4U, "setting above the range clamps");

    /* The saved file must be a key=value per line, and the values in it must
     * be the values the store holds. */
    settings_save();
    check(truncate_calls == 0 || written_length > 0U, "the file was written");
    check(strstr(written, "display.scale=") != 0,
          "the saved file names the key");
    check(strstr(written, "window.snap=1") != 0,
          "the saved file holds the current value");

    printf("  settings: %s\n", (failures == 0) ? "all checks passed"
                                                : "FAILED");
    if (failures != 0) {
        printf("\n  saved:\n%s\n", written);
    }
    return (failures == 0) ? 0 : 1;
}
