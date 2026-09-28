#ifndef KLYE_DOOM_LEVEL_H
#define KLYE_DOOM_LEVEL_H

#include <stdbool.h>
#include <stdint.h>

/* Level data structures, as Doom stores them.
 *
 * These are the on-disk records decoded into host integers, which is what the
 * renderer walks.  Nothing is transformed: Doom's angles are 16-bit binary
 * angles, heights are in 16-pixel units, and the coordinate space is the map's
 * own.  Converting to floating point happens at draw time, not here, so a
 * structure can be compared against the WAD byte for byte.
 *
 * Sizes are fixed by the format, so a level is a set of parallel arrays and
 * nothing needs allocating per lump beyond one block per array.
 */

struct doom_vertex {
    int16_t x;
    int16_t y;
};

struct doom_linedef {
    uint16_t start;      /* index into vertexes */
    uint16_t end;
    uint16_t flags;
    uint16_t special;
    uint16_t tag;
    int16_t right;       /* sidedef index, -1 when absent */
    int16_t left;
};

struct doom_sidedef {
    int16_t x_offset;
    int16_t y_offset;
    uint16_t upper;      /* patch names, 0 means none */
    uint16_t lower;
    uint16_t middle;
    uint16_t sector;
};

struct doom_sector {
    int16_t floor_height;
    int16_t ceiling_height;
    /* Both layouts are kept.  In a 1.9 WAD these are indices into PNAMES; in
     * the older shareware layout the same eight bytes are the texture name
     * itself, and DOOM1.WAD is the older one.  Which is which is decided once
     * per WAD by wad_is_version_199(), not per sector. */
    uint16_t floor_patch;
    uint16_t ceiling_patch;
    char floor_name[9];
    char ceiling_name[9];
    uint16_t light;
    uint16_t special;
    uint16_t tag;
};

struct doom_seg {
    uint16_t start;
    uint16_t end;
    int16_t angle;       /* 16-bit binary angle */
    uint16_t linedef;
    int16_t side;        /* 0 front, 1 back */
    int16_t offset;
};

struct doom_subsector {
    uint16_t count;      /* number of segs */
    uint16_t first;      /* index into segs */
};

struct doom_node {
    int16_t axis;        /* 0 x, 1 y */
    int16_t distance;    /* partition distance along the axis */
    uint16_t right;      /* node or subsector index */
    uint16_t left;
    int16_t right_box[4];
    int16_t left_box[4];
};

struct doom_level {
    char name[8];
    struct doom_vertex *vertexes;
    uint16_t vertex_count;
    struct doom_linedef *linedefs;
    uint16_t linedef_count;
    struct doom_sidedef *sidedefs;
    uint16_t sidedef_count;
    struct doom_sector *sectors;
    uint16_t sector_count;
    struct doom_seg *segs;
    uint16_t seg_count;
    struct doom_subsector *subsectors;
    uint16_t subsector_count;
    struct doom_node *nodes;
    uint16_t node_count;
    const uint8_t *reject;
    uint32_t reject_size;
    const int16_t *blockmap;
    uint32_t blockmap_words;
};

/* Load a level by its marker, such as "E1M1".  Needs a WAD already open.
 * The level borrows the WAD's memory for the reject table and blockmap, so it
 * stays valid only while that WAD is loaded. */
bool doom_level_load(const char *marker);

/* The level most recently loaded, or NULL. */
const struct doom_level *doom_level(void);

/* The PNAMES entry at an index, or "". */
const char *doom_level_patch_name(uint16_t index);

/* Resolve a sector's floor or ceiling texture to a patch name, whichever
 * layout the WAD uses.  Pass the sector and true for the ceiling.  Returns a
 * name, which may be a flat rather than a texture, or "" if it is unusable. */
const char *doom_level_floor_texture(const struct doom_sector *sector);
const char *doom_level_ceiling_texture(const struct doom_sector *sector);

/* Index of a patch name in PNAMES, or -1 when it is not a texture, in which
 * case it is a flat and has to be drawn differently. */
int doom_level_patch_index(const char *name);

/* Reason for the most recent failure, or "" if none. */
const char *doom_level_error(void);

#endif
