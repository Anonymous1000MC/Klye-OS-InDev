#ifndef KLYE_DOOM_H
#define KLYE_DOOM_H

#include <stdbool.h>
#include <stdint.h>

#include "gfx.h"

/* Doom's own graphics formats, decoded far enough to put real WAD content on
 * the screen.
 *
 * A patch is how Doom stores its pictures and sprites: a 8-byte header, a
 * column offset table, then per column a top delta and a run of posts.  Pixels
 * are indices into PLAYPAL, so a palette is needed before anything has a
 * colour.  Nothing is copied: pixels are written straight into a gfx surface
 * while a column is decoded, one column at a time, so a 320x200 picture does
 * not need a 320x200 staging buffer.
 */

#define DOOM_PALETTE_SIZE 256U
#define DOOM_PALETTE_COUNT 14U

/* Load a WAD from the disk image and take its first palette.  `path` is a name
 * in the image, such as "doom/DOOM1.WAD". */
bool doom_open_wad(const char *path);

/* True when a WAD is open. */
bool doom_wad_open(void);

/* Number of lumps in the open WAD. */
int doom_wad_lumps(void);

/* Palette index to 0x00RRGGBB.  Valid once a WAD is open. */
uint32_t doom_color(uint32_t index);

/* Switch palette.  `index` is clamped to the palettes the WAD actually has. */
void doom_set_palette(int index);

/* How many palettes the WAD has. */
int doom_palette_count(void);

/* Draw a patch lump at a position on a surface.  The patch's own left and top
 * offsets are applied, so passing 0,0 puts the patch's top left corner exactly
 * there.  Pixels outside the surface are clipped, not wrapped. */
bool doom_draw_patch(const char *lump, struct gfx_surface *surface, int x, int y);

/* Width and height of a patch lump, or 0 when it is not a patch. */
bool doom_patch_size(const char *lump, int *width, int *height);

/* Draw a full-screen picture scaled to fit a width, keeping the aspect ratio.
 * This is nearest neighbour, which is what Doom's own non-filtered mode does at
 * 1:1 and what looks right when a 320x200 original is shown larger. */
bool doom_draw_picture_scaled(const char *lump, struct gfx_surface *surface,
                              int x, int y, int target_width);

/* Reason for the most recent failure, or "" if none. */
const char * doom_error(void);

#endif
