#ifndef KLYE_WAD_H
#define KLYE_WAD_H

#include <stdbool.h>
#include <stdint.h>

/* WAD file access.
 *
 * A WAD is a 12-byte header, a directory of fixed-size entries, then the lump
 * data.  This parses the header and directory and hands out pointers into the
 * loaded image, with no copying: a lump is just an offset and a length into the
 * buffer that blob_load already produced, so reading a 4 MiB WAD costs one
 * buffer rather than one per lump.
 *
 * Offsets in a WAD are signed in the on-disk directory but a negative offset
 * means "the directory has no data section", which only happens in a WAD that
 * is nothing but an index.  Both are handled.
 */

#define WAD_LUMP_NAME_MAX 8

struct wad_lump {
    char name[WAD_LUMP_NAME_MAX + 1];
    uint32_t offset;   /* from the start of the lump data */
    uint32_t size;
    int32_t directory_index; /* position in the directory, -1 if synthesised */
    bool loaded;            /* false for the map markers that carry no data */
};

/* Parse a WAD already in memory.  `data` must stay valid for as long as the
 * lumps are used, because lumps point into it.  Returns false and fills in
 * wad_error() if the header or directory is not usable. */
bool wad_open(const void *data, uint32_t length);

/* Number of lumps in the directory. */
int wad_count(void);

/* Lump at `index`, or NULL when out of range. */
const struct wad_lump *wad_at(int index);

/* First lump with this name, or NULL.  WAD names are upper case and padded
 * with NULs, but the comparison is case-insensitive and ignores padding so
 * callers do not have to care. */
const struct wad_lump *wad_find(const char *name);

/* Compare a lump's stored name against a plain C string, ignoring case and
 * the padding.  A plain strcmp does not work: a stored name is always eight
 * characters, so "SEGS" is really "SEGS    " and never compares equal. */
bool wad_name_is(const char *name, const char *wanted);

/* Pointer to a lump's bytes, or NULL when the index is out of range or the
 * lump is a marker with no data. */
const void *wad_data(const struct wad_lump *lump);

/* Name of the lump marking the start of a level, or NULL.  Doom uses
 * E#M# to open and E#N# to close. */
const struct wad_lump *wad_level_start(const char *episode, int map);
const struct wad_lump *wad_level_next(const struct wad_lump *lump);

/* The three lumps that make up a level, in the order Doom reads them. */
bool wad_level_lumps(const char *episode, int map, const struct wad_lump **thngs,
                     const struct wad_lump **linedef, const struct wad_lump **sidedef);

/* True when a lump is a level marker, such as E1M1 or MAP07. */
bool wad_is_level_marker(const struct wad_lump *lump);

/* The three level data lumps belonging to a marker, found by name inside the
 * marker's block.  Any output pointer may be NULL. */
bool wad_level_parts(const struct wad_lump *marker, const struct wad_lump **thngs,
                     const struct wad_lump **linedef, const struct wad_lump **sidedef);

/* "IWAD" or "PWAD" for the loaded file, or "" when nothing is open. */
const char *wad_kind(void);

/* Whether the file uses the Doom 1.9 sector layout, where a sector's floor and
 * ceiling are indices into PNAMES, or the older layout where they are 8-byte
 * texture names.
 *
 * DOOM1.WAD, the shareware file, is the older one: it has no ANIMATED lump and
 * its sector fields literally spell "FLOOR4_8".  Reading those four bytes as
 * an index gives 19526, which is far outside PNAMES, so a renderer would look
 * up garbage.  The presence of ANIMATED is what distinguishes the two, and is
 * the same test the Doom source uses. */
bool wad_is_version_199(void);

/* Reason for the most recent failure, or "" if none. */
const char *wad_error(void);

#endif
