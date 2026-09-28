/* doom_level.c - Doom's level data structures, decoded from a WAD. */

#include "doom_level.h"
#include "heap.h"
#include "wad.h"

static struct doom_level level;
static bool level_loaded;
static char level_error[64] = "";

static void level_fail(const char *why)
{
    __builtin_strncpy(level_error, why, sizeof(level_error) - 1);
    level_error[sizeof(level_error) - 1] = 0;
}

static uint16_t get_u16(const void *at, uint32_t index)
{
    const uint8_t *bytes = (const uint8_t *)at + index * 2U;

    return (uint16_t)((uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8));
}

/* Every record in the format has a fixed size, so a lump that is not an exact
 * multiple of it is either corrupt or not the lump we think it is.  Checking
 * this catches a mis-identified lump immediately, rather than letting a
 * renderer walk off the end of a buffer later. */
static bool sized(const struct wad_lump *lump, uint32_t record, uint16_t *count)
{
    if (lump == 0 || !lump->loaded) {
        return false;
    }
    if (record == 0U || (lump->size % record) != 0U) {
        return false;
    }
    *count = (uint16_t)(lump->size / record);
    return true;
}

bool doom_level_load(const char *marker)
{
    const struct wad_lump *lump;
    const uint8_t *bytes;
    uint16_t count;
    int index;

    level_error[0] = 0;
    level_loaded = false;
    if (wad_count() == 0) {
        level_fail("no wad is open");
        return false;
    }
    lump = wad_find(marker);
    if (lump == 0) {
        level_fail("no such level");
        return false;
    }

    /* Walk the block after the marker, filling in whichever array each name
     * asks for.  The marker itself is the entry point. */
    index = lump->directory_index + 1;
    for (; index < wad_count(); ++index) {
        const struct wad_lump *data = wad_at(index);
        const char *name;

        if (data == 0) {
            break;
        }
        name = data->name;
        /* stop at the next level marker */
        if (wad_is_level_marker(data)) {
            break;
        }
        bytes = (const uint8_t *)wad_data(data);

        /* Decoded field by field rather than copied as a struct, because the
         * on-disk records are larger than the fields the renderer cares about:
         * a sidedef is 30 bytes but the six useful fields are 12, and a sector
         * is 26 against 14.  Copying the lump and indexing it as an array of
         * the smaller structs would stride through it wrongly. */
        if (wad_name_is(name, "VERTEXES") &&
            sized(data, 4U, &count)) {
            level.vertexes = heap_malloc((size_t)count * sizeof(*level.vertexes));
            if (level.vertexes == 0) {
                level_fail("out of memory for the vertexes");
                return false;
            }
            for (uint16_t at = 0; at < count; ++at) {
                level.vertexes[at].x = (int16_t)get_u16(bytes, at * 2U);
                level.vertexes[at].y = (int16_t)get_u16(bytes, at * 2U + 1U);
            }
            level.vertex_count = count;
        } else if (wad_name_is(name, "LINEDEFS") &&
                   sized(data, 14U, &count)) {
            level.linedefs = heap_malloc((size_t)count * sizeof(*level.linedefs));
            if (level.linedefs == 0) {
                level_fail("out of memory for the linedefs");
                return false;
            }
            for (uint16_t at = 0; at < count; ++at) {
                uint16_t *f = (uint16_t *)(bytes + (uint32_t)at * 14U);
                struct doom_linedef *out = &level.linedefs[at];

                out->start = f[0];
                out->end = f[1];
                out->flags = f[2];
                out->special = f[3];
                out->tag = f[4];
                /* 0xFFFF in a sidedef field means "no side", which is normal:
                 * a one sided line has no left side at all */
                out->right = (f[5] == 0xFFFFU) ? -1 : (int16_t)f[5];
                out->left = (f[6] == 0xFFFFU) ? -1 : (int16_t)f[6];
            }
            level.linedef_count = count;
        } else if (wad_name_is(name, "SIDEDEFS") &&
                   sized(data, 30U, &count)) {
            level.sidedefs = heap_malloc((size_t)count * sizeof(*level.sidedefs));
            if (level.sidedefs == 0) {
                level_fail("out of memory for the sidedefs");
                return false;
            }
            for (uint16_t at = 0; at < count; ++at) {
                const uint16_t *f = (const uint16_t *)(bytes + (uint32_t)at * 30U);
                struct doom_sidedef *out = &level.sidedefs[at];

                out->x_offset = (int16_t)f[0];
                out->y_offset = (int16_t)f[1];
                out->upper = f[2];
                out->lower = f[3];
                out->middle = f[4];
                out->sector = f[5];
            }
            level.sidedef_count = count;
        } else if (wad_name_is(name, "SEGS") &&
                   sized(data, 12U, &count)) {
            level.segs = heap_malloc((size_t)count * sizeof(*level.segs));
            if (level.segs == 0) {
                level_fail("out of memory for the segs");
                return false;
            }
            for (uint16_t at = 0; at < count; ++at) {
                const uint16_t *f = (const uint16_t *)(bytes + (uint32_t)at * 12U);
                struct doom_seg *out = &level.segs[at];

                out->start = f[0];
                out->end = f[1];
                out->angle = (int16_t)f[2];
                out->linedef = f[3];
                out->side = (int16_t)f[4];
                out->offset = (int16_t)f[5];
            }
            level.seg_count = count;
        } else if (wad_name_is(name, "SSECTORS") &&
                   sized(data, 4U, &count)) {
            level.subsectors = heap_malloc((size_t)count *
                                           sizeof(*level.subsectors));
            if (level.subsectors == 0) {
                level_fail("out of memory for the subsectors");
                return false;
            }
            for (uint16_t at = 0; at < count; ++at) {
                const uint16_t *f = (const uint16_t *)(bytes + (uint32_t)at * 4U);

                level.subsectors[at].count = f[0];
                level.subsectors[at].first = f[1];
            }
            level.subsector_count = count;
        } else if (wad_name_is(name, "NODES") &&
                   sized(data, 28U, &count)) {
            level.nodes = heap_malloc((size_t)count * sizeof(*level.nodes));
            if (level.nodes == 0) {
                level_fail("out of memory for the nodes");
                return false;
            }
            for (uint16_t at = 0; at < count; ++at) {
                const uint16_t *f = (const uint16_t *)(bytes + (uint32_t)at * 28U);
                struct doom_node *out = &level.nodes[at];

                for (int box = 0; box < 4; ++box) {
                    out->right_box[box] = (int16_t)f[box];
                    out->left_box[box] = (int16_t)f[4 + box];
                }
                out->right = f[8];
                out->left = f[9];
                out->axis = (int16_t)f[10];
                out->distance = (int16_t)f[11];
            }
            level.node_count = count;
        } else if (wad_name_is(name, "SECTORS") &&
                   sized(data, 26U, &count)) {
            level.sectors = heap_malloc((size_t)count * sizeof(*level.sectors));
            if (level.sectors == 0) {
                level_fail("out of memory for the sectors");
                return false;
            }
            for (uint16_t at = 0; at < count; ++at) {
                const uint16_t *f = (const uint16_t *)(bytes + (uint32_t)at * 26U);
                struct doom_sector *out = &level.sectors[at];

                out->floor_height = (int16_t)f[0];
                out->ceiling_height = (int16_t)f[1];
                out->floor_patch = f[2];
                out->ceiling_patch = f[3];
                /* the same eight bytes are also the texture name, which is what
                 * they mean in the pre-1.9 shareware layout */
                /* The inner variable must not be called at: it would shadow the
                 * sector index, every sector would read the same eight bytes,
                 * and all 85 of them came out as the same texture. */
                for (int slot = 0; slot < 8; ++slot) {
                    char c = (char)((const uint8_t *)bytes)[(uint32_t)at * 26U +
                                                            4U + (uint32_t)slot];

                    out->floor_name[slot] = (c >= 0x20 && c < 0x7F) ? c : 0;
                }
                out->floor_name[8] = 0;
                for (int slot = 0; slot < 8; ++slot) {
                    char c = (char)((const uint8_t *)bytes)[(uint32_t)at * 26U +
                                                            12U + (uint32_t)slot];

                    out->ceiling_name[slot] = (c >= 0x20 && c < 0x7F) ? c : 0;
                }
                out->ceiling_name[8] = 0;
                out->light = f[4];
                out->special = f[5];
                out->tag = f[6];
            }
            level.sector_count = count;
        } else if (wad_name_is(name, "REJECT") && data->loaded) {
            level.reject = bytes;
            level.reject_size = data->size;
        } else if (wad_name_is(name, "BLOCKMAP") && data->loaded) {
            level.blockmap = (const int16_t *)bytes;
            level.blockmap_words = data->size / 2U;
        }
    }

    if (level.vertexes == 0 || level.linedefs == 0 || level.sectors == 0 ||
        level.segs == 0) {
        /* name the one that is missing: "missing a structure" gave no way to
         * tell a rejected lump from a lump whose size did not divide evenly */
        level_fail(level.vertexes == 0 ? "no vertexes"
                     : level.linedefs == 0 ? "no linedefs"
                     : level.sectors == 0 ? "no sectors"
                                          : "no segs");
        return false;
    }
    for (int at = 0; at < 8; ++at) {
        level.name[at] = (at < 7) ? marker[at] : 0;
    }
    level.name[7] = 0;
    level_loaded = true;
    return true;
}

const char *doom_level_floor_texture(const struct doom_sector *sector)
{
    if (sector == 0) {
        return "";
    }
    if (wad_is_version_199()) {
        return doom_level_patch_name(sector->floor_patch);
    }
    return sector->floor_name;
}

const char *doom_level_ceiling_texture(const struct doom_sector *sector)
{
    if (sector == 0) {
        return "";
    }
    if (wad_is_version_199()) {
        return doom_level_patch_name(sector->ceiling_patch);
    }
    return sector->ceiling_name;
}

int doom_level_patch_index(const char *name)
{
    const struct wad_lump *lump = wad_find("PNAMES");
    uint32_t count;

    if (lump == 0 || !lump->loaded || lump->size < 4U) {
        return -1;
    }
    count = (lump->size - 4U) / 8U;
    for (uint32_t at = 0; at < count; ++at) {
        char entry[9];

        for (int byte = 0; byte < 8; ++byte) {
            entry[byte] = (char)((const uint8_t *)wad_data(lump))[4U + at * 8U +
                                                                     (uint32_t)byte];
        }
        entry[8] = 0;
        for (int byte = 7; byte >= 0 && entry[byte] == 0; --byte) {
            entry[byte] = ' ';
        }
        if (wad_name_is(entry, name)) {
            return (int)at;
        }
    }
    return -1;
}

const char *doom_level_patch_name(uint16_t index)
{
    static char name[9];
    const struct wad_lump *lump = wad_find("PNAMES");
    uint32_t count;

    name[0] = 0;
    if (lump == 0 || !lump->loaded || lump->size < 4U) {
        return name;
    }
    count = (lump->size - 4U) / 8U;
    if ((uint32_t)index >= count) {
        return name;
    }
    for (int byte = 0; byte < 8; ++byte) {
        name[byte] = (char)((const uint8_t *)wad_data(lump))[4U +
                                                               (uint32_t)index * 8U +
                                                               (uint32_t)byte];
    }
    name[8] = 0;
    for (int byte = 7; byte >= 0 && name[byte] == 0; --byte) {
        name[byte] = ' ';
    }
    return name;
}

const struct doom_level *doom_level(void)
{
    return level_loaded ? &level : 0;
}

const char *doom_level_error(void)
{
    return level_error;
}
