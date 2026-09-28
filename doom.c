/* doom.c - Doom patch decoding and palette handling.  See include/doom.h. */

#include "doom.h"
#include "blob.h"
#include "fat.h"
#include "wad.h"

static uint32_t doom_palette[DOOM_PALETTE_SIZE];
static void *doom_image;
static uint32_t doom_image_length;
static int doom_palettes;
static int doom_palette_current;
static char doom_error_text[64] = "";

/* The buffer came from whichever of the two filesystems supplied it, so it has
 * to go back the same way.  `from_fat` is set by whichever load succeeded. */
static bool doom_image_from_fat;

static void doom_image_release(void *pointer)
{
    blob_release_any(pointer, doom_image_from_fat);
}

static void doom_fail(const char *why)
{
    __builtin_strncpy(doom_error_text, why, sizeof(doom_error_text) - 1);
    doom_error_text[sizeof(doom_error_text) - 1] = 0;
}

static uint16_t doom_u16(const uint8_t *at)
{
    return (uint16_t)((uint16_t)at[0] | (uint16_t)((uint16_t)at[1] << 8));
}

static uint32_t doom_u32(const uint8_t *at)
{
    return (uint32_t)at[0] | ((uint32_t)at[1] << 8) | ((uint32_t)at[2] << 16) |
           ((uint32_t)at[3] << 24);
}

/* PLAYPAL is 14 palettes of 256 rgb triples, back to back.  Doom indexes into
 * the active one with the colormap for shading, which the patch decoder does
 * not need because it is drawing flat artwork. */
static bool doom_load_palette(const void *data, uint32_t length)
{
    const struct wad_lump *lump = wad_find("PLAYPAL");
    const uint8_t *bytes = (const uint8_t *)data;

    (void)length;
    if (lump == 0 || !lump->loaded) {
        doom_fail("the wad has no usable PLAYPAL");
        return false;
    }
    doom_palettes = (int)(lump->size / (DOOM_PALETTE_SIZE * 3U));
    if (doom_palettes < 1) {
        doom_fail("PLAYPAL is too small for even one palette");
        return false;
    }
    if (doom_palettes > (int)DOOM_PALETTE_COUNT) {
        doom_palettes = (int)DOOM_PALETTE_COUNT;
    }
    doom_palette_current = 0;
    for (uint32_t index = 0; index < DOOM_PALETTE_SIZE; ++index) {
        const uint8_t *entry = bytes + lump->offset + index * 3U;

        doom_palette[index] = ((uint32_t)entry[0] << 16) |
                              ((uint32_t)entry[1] << 8) | (uint32_t)entry[2];
    }
    return true;
}

bool doom_open_wad(const char *path)
{
    void *image = 0;
    uint32_t length = 0;

    doom_error_text[0] = 0;
    if (doom_image != 0) {
        doom_image_release(doom_image);
        doom_image = 0;
        doom_image_length = 0;
    }
    /* The FAT volume is a real filesystem and is tried first; the flat image is
     * only a fallback for a disk that has not been reformatted yet. */
    if (!blob_load_any(path, &image, &length, &doom_image_from_fat)) {
        doom_fail(blob_error());
        return false;
    }
    if (!wad_open(image, length)) {
        doom_image_release(image);
        doom_fail(wad_error());
        return false;
    }
    if (!doom_load_palette(image, length)) {
        doom_image_release(image);
        return false;
    }
    doom_image = image;
    doom_image_length = length;
    return true;
}

bool doom_wad_open(void)
{
    return doom_image != 0;
}

int doom_wad_lumps(void)
{
    return wad_count();
}

uint32_t doom_color(uint32_t index)
{
    return doom_palette[index & 0xFFU];
}

int doom_palette_count(void)
{
    return doom_palettes;
}

void doom_set_palette(int index)
{
    if (index < 0) {
        index = 0;
    }
    if (index >= doom_palettes) {
        index = doom_palettes - 1;
    }
    doom_palette_current = index;
    if (doom_image != 0) {
        const struct wad_lump *lump = wad_find("PLAYPAL");
        const uint8_t *bytes = (const uint8_t *)doom_image;
        const uint8_t *base = bytes + lump->offset +
                              (uint32_t)index * DOOM_PALETTE_SIZE * 3U;

        for (uint32_t at = 0; at < DOOM_PALETTE_SIZE; ++at) {
            const uint8_t *entry = base + at * 3U;

            doom_palette[at] = ((uint32_t)entry[0] << 16) |
                               ((uint32_t)entry[1] << 8) | (uint32_t)entry[2];
        }
    }
    (void)doom_palette_current;
}

static const uint8_t *doom_lump_bytes(const struct wad_lump *lump)
{
    if (lump == 0 || !lump->loaded) {
        return 0;
    }
    return (const uint8_t *)doom_image + lump->offset;
}

/* A patch is: int16 width, int16 height, int16 leftoffset, int16 topoffset,
 * int32 columnofs[width], then the columns.  Each column starts with a top
 * delta and then a run of posts, each a length byte followed by that many
 * palette indices, ending at 0xFF. */
bool doom_patch_size(const char *lump_name, int *width, int *height)
{
    const struct wad_lump *lump = wad_find(lump_name);
    const uint8_t *bytes = doom_lump_bytes(lump);

    if (bytes == 0) {
        doom_fail("no such patch, or it is a marker with no data");
        return false;
    }
    if (lump->size < 8U) {
        doom_fail("patch is too short for a header");
        return false;
    }
    if (width != 0) {
        *width = (int)doom_u16(bytes);
    }
    if (height != 0) {
        *height = (int)doom_u16(bytes + 2);
    }
    return true;
}

/* Doom stores its graphics in two different column layouts, and picking the
 * wrong one is not a subtle degradation.
 *
 * A picture, which is what a full screen graphic like TITLEPIC is, has every
 * column the same length, with the pixels packed at a fixed offset inside it:
 * two header bytes, `height` palette indices, some padding, and a 0xff to end
 * the column.  TITLEPIC is 320x200 with a stride of exactly 209, and 320 * 209
 * accounts for every byte of the lump after the offset table.
 *
 * A sprite, which is what a patch like TROOA1 is, has post encoded columns that
 * vary in length: a top delta, then runs of pixels each introduced by a length
 * byte, ending at 0xff.
 *
 * Read a picture as posts and the walk runs past the 0xff that ends the column
 * and starts treating the next column's bytes as more posts, so one column
 * paints hundreds of rows: TITLEPIC came out 320 pixels wide by 289 tall.  Read
 * a sprite as flat and almost all of it vanishes.  So the layout is detected
 * from the column table rather than assumed, and checked against the declared
 * height.
 *
 * A uniform stride that is too small to hold `height` pixels cannot be a
 * picture, which keeps a short patch with a single column from being mistaken
 * for one.
 */
static bool patch_is_flat(const uint8_t *bytes, const struct wad_lump *lump,
                          int width, int height)
{
    uint32_t first;
    uint32_t stride;

    if (width <= 0 || height <= 0 ||
        8U + (uint32_t)(width + 1) * 4U > lump->size) {
        return false;
    }
    first = doom_u32(bytes + 8);
    if (first < 8U + (uint32_t)width * 4U || first >= lump->size) {
        return false;
    }
    stride = width > 1 ? doom_u32(bytes + 12) - first : lump->size - first;
    if (stride < (uint32_t)height + 2U) {
        return false;
    }
    /* The span of column 0 is the stride above; this checks the rest.  The loop
     * has to stop at width - 1: on its last pass it would read one entry past
     * the table, which is the first bytes of the lump data and compares as a
     * nonsense span. */
    for (int column = 1; column < width - 1; ++column) {
        uint32_t here = doom_u32(bytes + 8 + (uint32_t)column * 4U);
        uint32_t after = doom_u32(bytes + 8 + (uint32_t)(column + 1) * 4U);

        if (here >= lump->size || after > lump->size ||
            after - here != stride) {
            return false;
        }
    }
    /* The last column has no following entry in the table: it runs to the end
     * of the lump.  Reading one past the table picks up the first bytes of the
     * lump data instead, which compares as a wildly wrong span and made this
     * report every picture as post encoded. */
    {
        uint32_t last = doom_u32(bytes + 8 + (uint32_t)(width - 1) * 4U);

        if (last >= lump->size || lump->size - last != stride) {
            return false;
        }
    }
    return true;
}

/* Draw a flat column: `height` palette indices at a fixed offset. */
static void doom_draw_flat_column(const uint8_t *bytes, uint32_t cursor,
                                  int surface_x, struct gfx_surface *surface,
                                  int base_y, int height, int step)
{
    for (int row = 0; row < height; ++row) {
        uint32_t color = doom_palette[bytes[cursor + 2 + (uint32_t)row]];

        for (int block_y = 0; block_y < step; ++block_y) {
            for (int block_x = 0; block_x < step; ++block_x) {
                gfx_pixel(surface, surface_x + block_x,
                          base_y + row * step + block_y, color);
            }
        }
    }
}

/* Walk one post encoded column.
 *
 * `cursor` is the absolute offset of the column's top delta byte, which is what
 * the patch's column table stores, and `column_end` is where the next column
 * starts.  The walk stops at that boundary as well as at a 0xff terminator, and
 * at the declared height, so a post length that does not add up can never paint
 * outside the picture.
 *
 * `step` spreads one source pixel over a step by step block, so a 320x200
 * picture can be shown larger with nearest neighbour, which is what Doom's own
 * unfiltered mode looks like.
 */
static void doom_draw_post_column(const uint8_t *bytes, uint32_t lump_size,
                                  uint32_t cursor, uint32_t column_end,
                                  int surface_x, int top_delta,
                                  struct gfx_surface *surface, int base_y,
                                  int height, int step)
{
    int at = top_delta;

    if (cursor >= column_end || cursor >= lump_size) {
        return;
    }
    ++cursor; /* past the top delta */

    for (;;) {
        uint8_t length;

        if (cursor >= column_end || cursor >= lump_size) {
            return;
        }
        length = bytes[cursor];
        ++cursor;
        if (length == 0xFFU) {
            return; /* end of this column */
        }
        if (length == 0U) {
            /* a zero length is a corrupt post, not a terminator; stepping over
             * it keeps a bad file from spinning here forever */
            continue;
        }
        if (cursor + length > column_end || cursor + length > lump_size) {
            /* this post claims more pixels than the column has room for, so
             * what follows is not column data at all */
            return;
        }
        for (uint8_t pixel = 0; pixel < length; ++pixel) {
            uint32_t color = doom_palette[bytes[cursor + pixel]];
            int row = at + (int)pixel;

            if (row < 0 || row >= height) {
                continue;
            }
            for (int block_y = 0; block_y < step; ++block_y) {
                for (int block_x = 0; block_x < step; ++block_x) {
                    gfx_pixel(surface, surface_x + block_x,
                              base_y + row * step + block_y, color);
                }
            }
        }
        cursor += length;
        at += (int)length;
        if (at >= height) {
            return;
        }
    }
}

bool doom_draw_patch(const char *lump_name, struct gfx_surface *surface,
                     int x, int y)
{
    const struct wad_lump *lump = wad_find(lump_name);
    const uint8_t *bytes = doom_lump_bytes(lump);
    int width;
    int height;
    int left;
    int top;

    if (bytes == 0) {
        doom_fail("no such patch, or it is a marker with no data");
        return false;
    }
    width = (int)doom_u16(bytes);
    height = (int)doom_u16(bytes + 2);
    left = (int)(int16_t)doom_u16(bytes + 4);
    top = (int)(int16_t)doom_u16(bytes + 6);
    if (width <= 0 || height <= 0 ||
        8U + (uint32_t)width * 4U > lump->size) {
        doom_fail("patch header does not agree with its size");
        return false;
    }
    {
        bool flat = patch_is_flat(bytes, lump, width, height);

        for (int column = 0; column < width; ++column) {
            uint32_t offset = doom_u32(bytes + 8 + (uint32_t)column * 4U);
            uint32_t next = doom_u32(bytes + 8 + (uint32_t)(column + 1) * 4U);
            int destination_x = x + column - left;

            if (column + 1 < width && (next <= offset || next > lump->size)) {
                next = lump->size;
            }
            if (offset >= lump->size) {
                continue;
            }
            if (flat) {
                doom_draw_flat_column(bytes, offset, destination_x, surface,
                                      y + top, height, 1);
            } else {
                doom_draw_post_column(bytes, lump->size, offset, next,
                                      destination_x, (int)bytes[offset],
                                      surface, y + top, height, 1);
            }
        }
    }
    return true;
}

bool doom_draw_picture_scaled(const char *lump_name,
                              struct gfx_surface *surface, int x, int y,
                              int target_width)
{
    const struct wad_lump *lump = wad_find(lump_name);
    const uint8_t *bytes = doom_lump_bytes(lump);
    int width;
    int height;
    int left;
    int top;
    int step;

    if (bytes == 0) {
        doom_fail("no such patch, or it is a marker with no data");
        return false;
    }
    width = (int)doom_u16(bytes);
    height = (int)doom_u16(bytes + 2);
    left = (int)(int16_t)doom_u16(bytes + 4);
    top = (int)(int16_t)doom_u16(bytes + 6);
    if (width <= 0 || height <= 0 ||
        8U + (uint32_t)width * 4U > lump->size) {
        doom_fail("patch header does not agree with its size");
        return false;
    }
    step = target_width / width;
    if (step < 1) {
        step = 1;
    }
    /* the patch's own top offset moves the whole picture down once, rather than
     * being folded into every row */
    y += top * step;
    {
        bool flat = patch_is_flat(bytes, lump, width, height);

        for (int column = 0; column < width; ++column) {
            uint32_t offset = doom_u32(bytes + 8 + (uint32_t)column * 4U);
            uint32_t next = doom_u32(bytes + 8 + (uint32_t)(column + 1) * 4U);
            int destination_x = x + column * step - left * step;

            if (column + 1 < width && (next <= offset || next > lump->size)) {
                next = lump->size;
            }
            if (offset >= lump->size) {
                continue;
            }
            if (flat) {
                doom_draw_flat_column(bytes, offset, destination_x, surface, y,
                                      height, step);
            } else {
                doom_draw_post_column(bytes, lump->size, offset, next,
                                      destination_x, (int)bytes[offset],
                                      surface, y, height, step);
            }
        }
    }
    return true;
}

const char *doom_error(void)
{
    return doom_error_text;
}
