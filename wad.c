/* wad.c - WAD header and directory parsing.  See include/wad.h. */

#include "wad.h"

#define WAD_DIRECTORY_ENTRY 16U
#define WAD_HEADER_BYTES 12U

/* Directory entries are 16 bytes, so eight fit in a page.  Doom 1 has 1264 of
 * them; the margin is for the larger commercial WADs. */
#define WAD_MAX_LUMPS 4096U

static const uint8_t *wad_base;
static uint32_t wad_length;
static struct wad_lump wad_lumps[WAD_MAX_LUMPS];
static int wad_lump_count;
static uint32_t wad_data_base;
static char wad_kind_text[5];
static char wad_error_text[64] = "";

static void wad_fail(const char *why)
{
    __builtin_strncpy(wad_error_text, why, sizeof(wad_error_text) - 1);
    wad_error_text[sizeof(wad_error_text) - 1] = 0;
}

/* Everything in a WAD is little-endian, and the kernel is built
 * little-endian, so a straight load works.  Going through these helpers keeps
 * that assumption in one place instead of scattering casts. */
static uint32_t wad_u32(const void *at)
{
    const uint8_t *bytes = (const uint8_t *)at;

    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static int32_t wad_i32(const void *at)
{
    return (int32_t)wad_u32(at);
}

static char wad_upper(char character)
{
    if (character >= 'a' && character <= 'z') {
        return (char)(character - ('a' - 'A'));
    }
    return character;
}

static bool wad_name_matches(const char *stored, const char *wanted)
{
    int index = 0;

    for (;; ++index) {
        char left = stored[index];
        char right = wanted[index];

        if (left == 0) {
            left = ' ';
        }
        if (right == 0) {
            right = ' ';
        }
        if (left == ' ' || right == ' ') {
            /* a space in either name ends the comparison: WAD names are
             * NUL padded rather than space padded, and some editors pad with
             * spaces instead */
            return true;
        }
        if (left != right && wad_upper(left) != wad_upper(right)) {
            return false;
        }
    }
}

bool wad_open(const void *data, uint32_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;
    int32_t lump_total;
    int32_t directory_offset;
    uint32_t needed;

    wad_error_text[0] = 0;
    wad_base = bytes;
    wad_length = length;
    wad_lump_count = 0;
    wad_data_base = 0;
    wad_kind_text[0] = 0;

    if (length < WAD_HEADER_BYTES) {
        wad_fail("file is too short to hold a wad header");
        return false;
    }
    for (int index = 0; index < 4; ++index) {
        wad_kind_text[index] = (char)bytes[index];
    }
    wad_kind_text[4] = 0;
    if (!(__builtin_strcmp(wad_kind_text, "IWAD") == 0 ||
          __builtin_strcmp(wad_kind_text, "PWAD") == 0)) {
        wad_fail("not a wad: bad magic");
        return false;
    }

    lump_total = wad_i32(bytes + 4);
    directory_offset = wad_i32(bytes + 8);
    if (lump_total <= 0 || (uint32_t)lump_total > WAD_MAX_LUMPS) {
        wad_fail("implausible lump count");
        return false;
    }
    if (directory_offset < 0) {
        /* an index-only WAD: no lump data at all, so nothing can be read out
         * of it, but the directory is still worth having */
        wad_fail("wad has a directory but no data section");
        return false;
    }

    needed = (uint32_t)directory_offset +
             (uint32_t)lump_total * WAD_DIRECTORY_ENTRY;
    if (needed > length) {
        wad_fail("wad directory runs past the end of the file");
        return false;
    }

    /* The offsets in a lump's directory entry are absolute file offsets, not
     * offsets from the start of the lump data.  In DOOM1.WAD the directory sits
     * at the very end of the file, so the two readings differ by four
     * megabytes and adding the data base pushed every lump past the end. */
    for (int32_t index = 0; index < lump_total; ++index) {
        const uint8_t *entry = bytes + (uint32_t)directory_offset +
                               (uint32_t)index * WAD_DIRECTORY_ENTRY;
        struct wad_lump *lump = &wad_lumps[index];
        int32_t stored_offset = wad_i32(entry);
        uint32_t size = wad_u32(entry + 4);

        for (int at = 0; at < WAD_LUMP_NAME_MAX; ++at) {
            lump->name[at] = (char)entry[8 + at];
        }
        lump->name[WAD_LUMP_NAME_MAX] = 0;
        for (int at = WAD_LUMP_NAME_MAX - 1; at >= 0; --at) {
            if (lump->name[at] == 0) {
                lump->name[at] = ' ';
            }
        }
        lump->size = size;
        lump->directory_index = index;
        /* A marker such as E1M1 has a zero offset and size.  Doom treats those
         * as having no data rather than as pointing at the start. */
        lump->loaded = stored_offset >= 0 && size != 0U &&
                       (uint64_t)stored_offset + size <= length;
        if (stored_offset < 0) {
            lump->offset = 0;
        } else {
            lump->offset = (uint32_t)stored_offset;
        }
    }
    wad_lump_count = (int)lump_total;
    return true;
}

int wad_count(void)
{
    return wad_lump_count;
}

const struct wad_lump *wad_at(int index)
{
    if (index < 0 || index >= wad_lump_count) {
        return 0;
    }
    return &wad_lumps[index];
}

const struct wad_lump *wad_find(const char *name)
{
    for (int index = 0; index < wad_lump_count; ++index) {
        if (wad_name_matches(wad_lumps[index].name, name)) {
            return &wad_lumps[index];
        }
    }
    return 0;
}

const void *wad_data(const struct wad_lump *lump)
{
    if (lump == 0 || !lump->loaded) {
        return 0;
    }
    return wad_base + lump->offset;
}

/* Is this a level marker, whatever the episode is called?  Names are padded
 * with NULs to 8 characters here, so check the first four. */
static bool wad_is_marker_name(const char *name)
{
    if (name[4] != 0 && name[4] != ' ') {
        return false;
    }
    if (name[0] == 'E' && name[2] == 'M' && name[3] >= '0' && name[3] <= '9') {
        return true;
    }
    if (name[0] == 'M' && name[1] == 'A' && name[2] == 'P' && name[3] >= '0' &&
        name[3] <= '9') {
        return true;
    }
    return false;
}

const char *wad_kind(void)
{
    return wad_kind_text;
}

bool wad_is_level_marker(const struct wad_lump *lump)
{
    if (lump == 0) {
        return false;
    }
    return wad_is_marker_name(lump->name);
}

/* Level markers are named E1M1 through E1M9 for maps below ten, and MAP01
 * upwards once there are more than ten of them, so the map number is not
 * zero padded.  Padding it turned E1M1 into E01M1 and matched nothing. */
static void wad_level_name(char *out, int length, const char *episode, int map)
{
    int at = 0;

    if (map >= 10) {
        for (int index = 0; episode[index] != 0 && at < length - 4; ++index) {
            out[at++] = wad_upper(episode[index]);
        }
        out[at++] = 'A'; /* "MAP01", "MAP10" */
        out[at++] = 'P';
        out[at++] = (char)('0' + (map / 10) % 10);
        out[at++] = (char)('0' + map % 10);
        out[at] = 0;
        return;
    }
    for (int index = 0; episode[index] != 0 && at < length - 3; ++index) {
        out[at++] = wad_upper(episode[index]);
    }
    out[at++] = 'M';
    out[at++] = (char)('0' + map % 10);
    out[at] = 0;
}

const struct wad_lump *wad_level_start(const char *episode, int map)
{
    char name[6];

    wad_level_name(name, (int)sizeof(name), episode, map);
    return wad_find(name);
}


const struct wad_lump *wad_level_next(const struct wad_lump *lump)
{
    if (lump == 0) {
        return 0;
    }
    /* the closing marker of this level, e.g. E1N1 is followed by E1N2 */
    int index = lump->directory_index;

    if (index < 0 || index + 1 >= wad_lump_count) {
        return 0;
    }
    return &wad_lumps[index + 1];
}

/* Locate the three level data lumps after a marker.  Shares the body with
 * wad_level_lumps, which just has to find the marker first. */
static bool wad_level_parts_at(int index, const struct wad_lump **thngs,
                               const struct wad_lump **linedef,
                               const struct wad_lump **sidedef)
{
    int end;
    int found = 0;

    if (index < 0 || index >= wad_lump_count) {
        return false;
    }

    /* A level is the marker followed by its data lumps, up to the next marker
     * or the end of the directory.  Doom's own order is marker, THINGS,
     * LINEDEFS, SIDEDEFS, VERTEXES, SEGS, SSECTORS, NODES, SECTORS, REJECT,
     * BLOCKMAP, but looking each one up by name inside the block is what keeps
     * this from depending on that being exactly right. */
    /* find the end of this level's block */
    end = wad_lump_count;
    for (int scan = index + 1; scan < wad_lump_count; ++scan) {
        if (wad_is_marker_name(wad_lumps[scan].name)) {
            end = scan;
            break;
        }
    }

    for (int scan = index + 1; scan < end; ++scan) {
        if (wad_name_matches(wad_lumps[scan].name, "THINGS") && thngs != 0) {
            *thngs = &wad_lumps[scan];
            ++found;
        } else if (wad_name_matches(wad_lumps[scan].name, "LINEDEFS") &&
                   linedef != 0) {
            *linedef = &wad_lumps[scan];
            ++found;
        } else if (wad_name_matches(wad_lumps[scan].name, "SIDEDEFS") &&
                   sidedef != 0) {
            *sidedef = &wad_lumps[scan];
            ++found;
        }
    }
    return found != 0;
}

bool wad_level_parts(const struct wad_lump *marker, const struct wad_lump **thngs,
                     const struct wad_lump **linedef, const struct wad_lump **sidedef)
{
    if (marker == 0) {
        return false;
    }
    return wad_level_parts_at(marker->directory_index, thngs, linedef, sidedef);
}

bool wad_level_lumps(const char *episode, int map, const struct wad_lump **thngs,
                     const struct wad_lump **linedef, const struct wad_lump **sidedef)
{
    const struct wad_lump *marker = wad_level_start(episode, map);

    if (marker == 0) {
        return false;
    }
    return wad_level_parts_at(marker->directory_index, thngs, linedef, sidedef);
}

const char *wad_error(void)
{
    return wad_error_text;
}
