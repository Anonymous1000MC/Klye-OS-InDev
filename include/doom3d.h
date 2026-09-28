#ifndef KLYE_DOOM3D_H
#define KLYE_DOOM3D_H

#include <stdbool.h>
#include <stdint.h>

#include "gfx.h"

/* A 3D view of a loaded level, without textures.
 *
 * The geometry comes entirely from the level data that doom_level.c already
 * decodes and verifies, so this works on a WAD whatever its texture format
 * turns out to be.  That matters: the texture layout of DOOM1.WAD is not the
 * vanilla one and is still unresolved, and a view that needs no textures is
 * the part of the renderer that is ready now.
 *
 * Visibility is a ray cast per screen column rather than a BSP walk, which is
 * the one design decision worth spelling out.  Casting a ray per column is the
 * older way to do this and is much simpler than clipping polygons, and it is
 * simpler for a specific reason rather than by accident: the floor and
 * ceiling of the subsector the player is standing in contain the eye, and
 * clipping such a polygon against the near plane does not produce a convex
 * shape, so painter's algorithm has nothing to draw.  A ray has no such
 * problem, and it gives the same answer for a level made of convex sectors.
 *
 * Cost is one pass over the segs per column, so a full screen is a few hundred
 * thousand integer operations, which is nothing next to the texture decoding
 * this will eventually need anyway.
 */

/* Sector heights are already in map units: a ceiling is around 128 and a
 * player is 56, so the eye sits 41 above the floor, exactly as Doom's
 * VIEWHEIGHT says.  Getting this wrong by a factor of 16 puts the viewer above
 * the ceiling and the view comes out nearly empty. */
#define DOOM3D_EYE_HEIGHT 41

/* Near plane, in map units.  Small, because a Doom sector is only tens of
 * units across and a player standing against a wall still has to see it. */
#define DOOM3D_NEAR 8

struct doom3d_stats {
    uint16_t segs;      /* segs in the render list */
    uint16_t solid;     /* of those, the ones that block a ray */
    uint16_t columns;   /* columns that hit something */
    uint16_t open;      /* columns that saw no geometry at all */
    uint32_t pixels;    /* pixels written */
};

/* Build the renderable geometry from the level doom_level_load() just opened.
 * Must be called again after loading a different level. */
bool doom3d_prepare(void);

/* Throw away geometry built for a previous level.  A seg list points into the
 * level's arrays, so loading a new level without this renders stale geometry
 * from the old one, or reads freed memory. */
void doom3d_reset(void);

/* Put the viewer somewhere.  `angle_deg` is clockwise from +x, and the eye is
 * DOOM3D_EYE_HEIGHT above `floor`. */
void doom3d_set_view(int32_t x, int32_t y, int32_t angle_deg, int32_t floor);

/* Turn on the spot, and walk by a map space offset.  Walking stops short of
 * any wall within DOOM3D_RADIUS, and picks up the floor of whatever sector it
 * ends up in, so stepping off a ledge drops the eye. */
void doom3d_turn(int32_t degrees);
void doom3d_step(int32_t dx, int32_t dy);

/* Walk `distance` along or against the way the viewer faces.  A script driving
 * a player should not have to work out a heading itself, and the kernel's Lua
 * has no trigonometry to offer it. */
void doom3d_walk(int32_t distance);

/* The radius a walking viewer keeps from a wall, in map units. */
#define DOOM3D_RADIUS 16

/* Current viewer state, which a game needs to save. */
void doom3d_get_view(int32_t *x, int32_t *y, int32_t *angle, int32_t *floor);

/* How many segs are in the render list, and how many of them are solid. */
uint16_t doom3d_seg_count(void);
uint16_t doom3d_solid_count(void);

/* The sector a subsector belongs to, or -1.  Exposed because it is the one
 * piece of level interpretation that is easy to get backwards: a back side
 * seg takes its sector from the linedef's left sidedef, not the right. */
int doom3d_subsector_sector(uint16_t index);

/* Draw the view into the rectangle at (x, y) of a surface, which may be any
 * size and any position, because a window is a rectangle inside a desktop and
 * not a surface of its own.  A 90 degree horizontal field of view is assumed.
 * `void_color` fills in the columns that hit no geometry.  `out` may be NULL.
 *
 * The view is drawn in place rather than through the display list the rest of
 * the script uses, so a caller replaying a display list has to treat it as a
 * command: it has to be in the list, or the compositor never asks for it, and
 * it has to carry the viewer position and angle, or the list looks unchanged
 * and a moving player sees a frozen room. */
bool doom3d_render(struct gfx_surface *surface, int x, int y, int width,
                   int height, uint32_t void_color, struct doom3d_stats *out);

/* Reason for the most recent failure, or "" if none. */
const char *doom3d_error(void);

#endif
