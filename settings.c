/* Settings, with somewhere to put them.
 *
 * The Settings application used to be a picture of a settings application:
 * six categories down the side, one of them highlighted, and a panel of labels
 * with fixed values.  Nothing in it was wired to anything, and the values were
 * whatever the drawing code said rather than what the system was doing.
 *
 * This is the part that makes it a program rather than a picture.  The values
 * live here, they are read at startup, and writing one saves the file.
 *
 * The file is plain text in the filesystem, one "key=value" per line.  Plain
 * because a person may need to edit it, and because a format with no parser is
 * a format with no parser.  Read and written whole -- the file is a few hundred
 * bytes and a partial write would leave a truncated last line rather than
 * something loadable.
 */

#include "settings.h"

#include <stdbool.h>
#include <stdint.h>

#include "vfs.h"

#define SETTINGS_FILE "/settings.conf"
#define SETTINGS_FILE_MAX 2048
#define SETTINGS_LINE_MAX 96

struct settings_value {
    const char *key;
    uint32_t value;
    uint32_t low;
    uint32_t high;
};

static struct settings_value settings_values[] = {
    /* The frame rate is reported, not set: the panel says what the machine is
     * doing, and a setting that claimed otherwise would be a lie on the
     * settings page as well as on the boot screen. */
    { "compositor.fps_limit", 240U, 30U, 240U },
    { "compositor.fps_window", 0U, 0U, 1000U },
    { "display.scale", 1U, 1U, 4U },
    { "display.wallpaper", 0U, 0U, 1U },
    { "theme.accent", 0U, 0U, 7U },
    { "window.snap", 1U, 0U, 1U },
    { "window.minimum_width", 240U, 120U, 900U },
    { "window.minimum_height", 120U, 80U, 700U },
    { "terminal.lines", 400U, 50U, 2000U },
    { "terminal.columns", 108U, 20U, 400U },
    { "terminal.cursor_blink", 1U, 0U, 1U },
    { "sound.enabled", 0U, 0U, 1U },
    { "boot.show_banner", 1U, 0U, 1U },
};

static const uint32_t settings_count =
    (uint32_t)(sizeof(settings_values) / sizeof(settings_values[0]));

static bool settings_loaded;

static struct settings_value *settings_find(const char *key)
{
    for (uint32_t index = 0U; index < settings_count; ++index) {
        const char *have = settings_values[index].key;
        uint32_t at = 0U;

        while (have[at] != '\0' && key[at] != '\0' && have[at] == key[at]) {
            at++;
        }
        if (have[at] == '\0' && key[at] == '\0') {
            return &settings_values[index];
        }
    }
    return 0;
}

static uint32_t settings_parse(const char *text, uint32_t length)
{
    uint32_t value = 0U;
    uint32_t digits = 0U;

    for (uint32_t index = 0U; index < length && digits < 9U; ++index) {
        char c = text[index];

        if (c < '0' || c > '9') {
            break;
        }
        value = value * 10U + (uint32_t)(c - '0');
        digits++;
    }
    return value;
}

static uint32_t settings_format(uint32_t value, char *out, uint32_t max)
{
    char digits[12];
    uint32_t length = 0U;
    uint32_t at = 0U;

    if (max < 2U) {
        return 0U;
    }
    if (value == 0U) {
        out[0] = '0';
        out[1] = '\0';
        return 1U;
    }
    while (value > 0U && length < 11U) {
        digits[length++] = (char)('0' + value % 10U);
        value /= 10U;
    }
    while (length > 0U && at + 1U < max) {
        out[at++] = digits[--length];
    }
    out[at] = '\0';
    return at;
}

/* One line at a time out of a buffer of the whole file. */
static void settings_parse_text(const char *text, uint32_t length)
{
    uint32_t at = 0U;

    while (at < length) {
        uint32_t start = at;
        uint32_t stop;
        char key[SETTINGS_LINE_MAX];
        uint32_t key_length = 0U;
        struct settings_value *entry;

        while (at < length && text[at] != '\n') {
            at++;
        }
        stop = at;
        if (at < length) {
            at++;  /* past the newline */
        }
        if (stop > start && text[stop - 1U] == '\r') {
            stop--;
        }
        /* A line is "key=value".  Anything else, including a blank line and a
         * comment, is skipped rather than treated as a broken key. */
        while (start < stop && key_length < SETTINGS_LINE_MAX - 1U &&
               text[start] != '=') {
            if (text[start] != ' ') {
                key[key_length++] = text[start];
            }
            start++;
        }
        key[key_length] = '\0';
        if (start >= stop || text[start] != '=') {
            continue;
        }
        entry = settings_find(key);
        if (entry != 0) {
            /* Clamped on the way in.  A file with a nonsense value in it
             * should not be able to make a window four pixels wide. */
            uint32_t value = settings_parse(&text[start + 1U], stop - start - 1U);

            if (value < entry->low) {
                value = entry->low;
            }
            if (value > entry->high) {
                value = entry->high;
            }
            entry->value = value;
        }
    }
}

void settings_load(void)
{
    char text[SETTINGS_FILE_MAX];
    int got;

    if (settings_loaded) {
        return;
    }
    settings_loaded = true;
    got = vfs_read(SETTINGS_FILE, text, (uint32_t)sizeof(text) - 1U);
    if (got > 0) {
        text[got] = '\0';
        settings_parse_text(text, (uint32_t)got);
    }
    /* No file is not an error.  The defaults are the settings, and the file is
     * only there to change them from what they are by default. */
}

void settings_save(void)
{
    char text[SETTINGS_FILE_MAX];
    uint32_t at = 0U;

    for (uint32_t index = 0U; index < settings_count; ++index) {
        const char *key = settings_values[index].key;
        uint32_t key_length = 0U;

        while (key[key_length] != '\0') {
            key_length++;
        }
        if (at + key_length + 16U > (uint32_t)sizeof(text)) {
            break;
        }
        for (uint32_t at2 = 0U; at2 < key_length; ++at2) {
            text[at++] = key[at2];
        }
        text[at++] = '=';
        at += settings_format(settings_values[index].value, &text[at],
                              (uint32_t)sizeof(text) - at);
        text[at++] = '\n';
    }
    /* Truncate first: writing over a longer old file leaves the tail of it in
     * place, and a settings file with two copies of a key takes whichever the
     * parser reaches last, which is the one nobody edited. */
    if (vfs_truncate(SETTINGS_FILE) < 0 && vfs_touch(SETTINGS_FILE) < 0) {
        return;  /* read-only image: the settings simply do not persist */
    }
    (void)vfs_write(SETTINGS_FILE, text, at);
}

uint32_t settings_get(const char *key)
{
    struct settings_value *entry;

    settings_load();
    entry = settings_find(key);
    return (entry == 0) ? 0U : entry->value;
}

/* Returns whether the value actually changed, so a caller holding down a key
 * that repeats does not rewrite the file a hundred times a second. */
bool settings_set(const char *key, uint32_t value)
{
    struct settings_value *entry;

    settings_load();
    entry = settings_find(key);
    if (entry == 0) {
        return false;
    }
    if (value < entry->low) {
        value = entry->low;
    }
    if (value > entry->high) {
        value = entry->high;
    }
    if (entry->value == value) {
        return false;
    }
    entry->value = value;
    settings_save();
    return true;
}

bool settings_valid(const char *key)
{
    settings_load();
    return settings_find(key) != 0;
}

uint32_t settings_count_entries(void)
{
    return settings_count;
}

const char *settings_key_at(uint32_t index)
{
    return (index < settings_count) ? settings_values[index].key : 0;
}

uint32_t settings_value_at(uint32_t index)
{
    return (index < settings_count) ? settings_values[index].value : 0U;
}

uint32_t settings_low_at(uint32_t index)
{
    return (index < settings_count) ? settings_values[index].low : 0U;
}

uint32_t settings_high_at(uint32_t index)
{
    return (index < settings_count) ? settings_values[index].high : 0U;
}
