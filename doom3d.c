/* doom3d.c - a 3D view of a Doom level, with no textures.
 *
 * The geometry is already decoded and verified by doom_level.c, so this only
 * has to turn it into something on screen.  Textures are deliberately absent:
 * the texture layout of DOOM1.WAD is not the vanilla one and is still an open
 * question, and a view that does not need it can be finished and tested now
 * instead of waiting on that.
 *
 * Everything is integer.  The kernel has no libm, so the view rotation comes
 * from CORDIC rather than sin and cos, and each column divides a few times.
 * That also means the render is bit for bit reproducible, which is what makes
 * it testable on the host.
 */

#include "doom3d.h"

#include <stdlib.h>

#include "doom_level.h"
#include "heap.h"

/* A seg in the render list.  The two sectors it separates are kept because a
 * ray that crosses a two sided line moves into the sector behind it. */
struct seg {
    int32_t x1, y1, x2, y2;
    int16_t floor;
    int16_t ceiling;
    uint16_t front;   /* the sector this seg faces into */
    uint16_t back;    /* the other side, or DOOM3D_NO_SECTOR when one sided */
    bool solid;       /* blocks a ray: one sided, or closed on itself */
};

#define DOOM3D_NO_SECTOR 0xFFFFU

static struct seg *segs;
static uint16_t seg_count;
static uint16_t seg_cap;
static uint16_t solid_count;

static int32_t view_x, view_y, view_angle, view_floor, view_z;
static bool prepared;
static char error_text[64] = "";

static void fail(const char *why)
{
    __builtin_strncpy(error_text, why, sizeof(error_text) - 1);
    error_text[sizeof(error_text) - 1] = 0;
}

/* ------------------------------------------------------------------ trig --
 *
 * CORDIC.  Angles are in Doom's own binary angle, 65536 to the turn, which is
 * the unit the seg angles in the level already use, so nothing has to convert.
 * atan(2^-i) in those units, rounded.
 */
static const int32_t atan_table[16] = {
    8192, 4836, 2555, 1297, 651, 326, 163, 82,
    41,   20,   10,   5,    3,   1,   1,   0,
};

/* Direction the viewer faces, as sin and cos scaled by 65536.
 *
 * CORDIC converges from the origin, so the angle has to be folded into the
 * first octant and the result rotated back.  Three things about that fold are
 * easy to get wrong and all three are silent, because the function still
 * returns plausible numbers:
 *
 *  - folding only into a half turn saturates everything past 90 degrees, and
 *    returns the same answer for 135 and for 180;
 *  - the two 45 degree branches fold in opposite directions, so they need
 *    opposite inverse rotations, and reusing one of them flips the sign of
 *    everything on one side of 90;
 *  - subtracting a whole turn to bring the angle into range needs no sign
 *    change at all, while subtracting half a turn does.
 */
static void rotate(int32_t angle, int32_t *sin_out, int32_t *cos_out)
{
    int32_t x = 39797; /* 0.60725 * 65536, the usual CORDIC gain */
    int32_t y = 0;
    int32_t z = angle & 0xFFFF;
    int32_t cos_v;
    int32_t sin_v;
    int32_t held;
    bool negate = false;
    int quarter = 0;
    int i;

    if (z >= 32768) {
        z -= 65536; /* a whole turn: the same direction, no sign to carry */
    }
    if (z > 16384 || z < -16384) {
        z += (z > 0) ? -32768 : 32768; /* half a turn: sign flips */
        negate = !negate;
    }
    if (z > 8192) {
        z -= 16384;
        quarter = 1; /* folded back by 90, so restore it afterwards */
    } else if (z < -8192) {
        z += 16384;
        quarter = -1;
    }

    for (i = 0; i < 16; i++) {
        int32_t nx;
        int32_t ny;

        if (z >= 0) {
            nx = x - (y >> i);
            ny = y + (x >> i);
            z -= atan_table[i];
        } else {
            nx = x + (y >> i);
            ny = y - (x >> i);
            z += atan_table[i];
        }
        x = nx;
        y = ny;
    }
    cos_v = x;
    sin_v = y;
    if (quarter > 0) {
        held = cos_v;
        cos_v = -sin_v;
        sin_v = held;
    } else if (quarter < 0) {
        held = cos_v;
        cos_v = sin_v;
        sin_v = -held;
    }
    if (negate) {
        cos_v = -cos_v;
        sin_v = -sin_v;
    }
    *cos_out = cos_v;
    *sin_out = sin_v;
}

/* Integer square root.  Only ever called a handful of times per frame, once
 * per walking step, so the plain bit by bit version is the right amount of
 * machinery. */
static int32_t isqrt(uint64_t value)
{
    uint64_t rem = 0;
    uint64_t root = 0;
    int i;

    for (i = 0; i < 32; i++) {
        root <<= 1;
        rem = (rem << 2) | (value >> 62);
        value <<= 2;
        if (root < rem) {
            rem -= root | 1;
            root += 2;
        }
    }
    return (int32_t)(root >> 1);
}

/* ------------------------------------------------------------- geometry --
 *
 * Which sector a seg belongs to.  A front side seg takes the sector from the
 * linedef's right sidedef and a back side seg from its left, which is the one
 * piece of level interpretation that is easy to get backwards; taking the
 * right one for both paints half of every room as its neighbour. */
static int seg_sector(const struct doom_level *lv, const struct doom_seg *seg)
{
    const struct doom_linedef *line;
    int16_t side;

    if (seg->linedef >= lv->linedef_count) {
        return -1;
    }
    line = &lv->linedefs[seg->linedef];
    side = (seg->side == 0) ? line->right : line->left;
    if (side < 0 || side >= lv->sidedef_count) {
        return -1;
    }
    return lv->sidedefs[side].sector;
}

static int seg_other_sector(const struct doom_level *lv,
                            const struct doom_seg *seg)
{
    const struct doom_linedef *line;
    int16_t other;

    if (seg->linedef >= lv->linedef_count) {
        return -1;
    }
    line = &lv->linedefs[seg->linedef];
    other = (seg->side == 0) ? line->left : line->right;
    if (other < 0 || other >= lv->sidedef_count) {
        return -1;
    }
    return lv->sidedefs[other].sector;
}

bool doom3d_prepare(void)
{
    const struct doom_level *lv = doom_level();
    uint16_t i;

    if (prepared) {
        return true;
    }
    if (lv == 0) {
        fail("no level is loaded");
        return false;
    }

    seg_cap = lv->seg_count;
    segs = heap_calloc(seg_cap ? seg_cap : 1, sizeof(*segs));
    if (segs == 0) {
        fail("out of memory building geometry");
        return false;
    }
    seg_count = 0;
    solid_count = 0;

    for (i = 0; i < lv->seg_count; i++) {
        const struct doom_seg *src = &lv->segs[i];
        struct seg *out;
        int front;
        int other;

        if (seg_count >= seg_cap) {
            break;
        }
        if (src->start >= lv->vertex_count || src->end >= lv->vertex_count) {
            continue;
        }
        front = seg_sector(lv, src);
        if (front < 0 || front >= lv->sector_count) {
            continue;
        }
        other = seg_other_sector(lv, src);

        out = &segs[seg_count++];
        out->x1 = lv->vertexes[src->start].x;
        out->y1 = lv->vertexes[src->start].y;
        out->x2 = lv->vertexes[src->end].x;
        out->y2 = lv->vertexes[src->end].y;
        out->front = (uint16_t)front;
        out->back = (other >= 0 && other < lv->sector_count)
                        ? (uint16_t)other
                        : DOOM3D_NO_SECTOR;
        out->floor = lv->sectors[front].floor_height;
        out->ceiling = lv->sectors[front].ceiling_height;
        /* Two sides naming different sectors is a doorway, and a ray passes
         * through it rather than stopping.  One sided, or both sides naming the
         * same sector, is a wall. */
        out->solid = (out->back == DOOM3D_NO_SECTOR || out->back == out->front);
        if (out->solid) {
            solid_count++;
        }
    }

    view_x = 0;
    view_y = 0;
    view_angle = 0;
    view_floor = 0;
    view_z = DOOM3D_EYE_HEIGHT;
    prepared = true;
    return true;
}

void doom3d_set_view(int32_t x, int32_t y, int32_t angle_deg, int32_t floor)
{
    view_x = x;
    view_y = y;
    view_angle = ((angle_deg % 360) + 360) % 360;
    view_floor = floor;
    view_z = floor + DOOM3D_EYE_HEIGHT;
}

uint16_t doom3d_seg_count(void)
{
    return seg_count;
}

void doom3d_reset(void)
{
    if (segs != 0) {
        heap_free(segs);
        segs = 0;
    }
    seg_count = 0;
    seg_cap = 0;
    solid_count = 0;
    prepared = false;
}

/* The sector containing a point, or -1.  A subsector is convex, so the test is
 * a sign check across its edges, and 237 of them per query is not worth
 * caching for something a player does a few times a second. */
static int sector_at(int32_t x, int32_t y)
{
    const struct doom_level *lv = doom_level();
    uint16_t s;

    if (lv == 0) {
        return -1;
    }
    for (s = 0; s < lv->subsector_count; s++) {
        const struct doom_subsector *ss = &lv->subsectors[s];
        uint16_t k;
        int32_t sign = 0;
        bool inside = true;

        if (ss->count < 3U) {
            continue;
        }
        for (k = 0; k < ss->count; k++) {
            uint16_t a = (uint16_t)(ss->first + k);
            uint16_t b = (uint16_t)(ss->first + ((k + 1U) % ss->count));

            if (a >= lv->seg_count || b >= lv->seg_count) {
                inside = false;
                break;
            }
            {
                const struct doom_vertex *va = &lv->vertexes[lv->segs[a].start];
                const struct doom_vertex *vb = &lv->vertexes[lv->segs[b].start];
                int32_t ex = vb->x - va->x;
                int32_t ey = vb->y - va->y;
                int64_t side = (int64_t)ex * (y - va->y) -
                               (int64_t)ey * (x - va->x);

                if (side == 0) {
                    continue;
                }
                if (sign == 0) {
                    sign = (side > 0) ? 1 : -1;
                } else if ((side > 0 ? 1 : -1) != sign) {
                    inside = false;
                    break;
                }
            }
        }
        if (inside == false) {
            continue;
        }
        {
            int sector = seg_sector(lv, &lv->segs[ss->first]);

            if (sector >= 0) {
                return sector;
            }
        }
    }
    return -1;
}

void doom3d_turn(int32_t degrees)
{
    view_angle = (((view_angle + degrees) % 360) + 360) % 360;
}

/* Nearest solid wall in a direction, in map units, or 0 when the way is
 * clear.  This is the same intersection the renderer uses, factored out so
 * walking and drawing cannot disagree about where a wall is. */
static int32_t wall_ahead(int32_t dx, int32_t dy)
{
    int32_t best = 0;
    uint16_t i;

    if (dx == 0 && dy == 0) {
        return 0;
    }
    for (i = 0; i < seg_count; i++) {
        const struct seg *s = &segs[i];
        int32_t ax = s->x1 - view_x;
        int32_t ay = s->y1 - view_y;
        int32_t ex = s->x2 - s->x1;
        int32_t ey = s->y2 - s->y1;
        int64_t denom = (int64_t)dx * ey - (int64_t)dy * ex;
        int64_t numer;
        int64_t along;
        int64_t across;

        if (s->solid == false || denom == 0) {
            continue;
        }
        numer = (int64_t)ax * ey - (int64_t)ay * ex;
        if (numer == 0) {
            continue;
        }
        along = (numer << 16) / denom;
        if (along <= 0) {
            continue;
        }
        across = (((int64_t)ax * dy - (int64_t)ay * dx) << 16) / denom;
        if (across < 0 || across > 65536) {
            continue;
        }
        if (best == 0 || along < best) {
            best = (int32_t)along;
        }
    }
    return best;
}

void doom3d_step(int32_t dx, int32_t dy)
{
    int32_t want = 0;
    int32_t limit;
    int32_t sector;

    want = isqrt((uint64_t)((int64_t)dx * dx + (int64_t)dy * dy));
    if (want == 0) {
        return;
    }
    limit = wall_ahead(dx, dy);
    if (limit == 0) {
        view_x += dx;
        view_y += dy;
    } else if (limit > DOOM3D_RADIUS) {
        /* Stop just short of the wall rather than exactly on it, so the next
         * step does not start inside the seg it is about to test against. */
        int32_t allowed = limit - DOOM3D_RADIUS;

        if (allowed < want) {
            view_x += (int32_t)(((int64_t)dx * allowed) / want);
            view_y += (int32_t)(((int64_t)dy * allowed) / want);
        } else {
            view_x += dx;
            view_y += dy;
        }
    }

    sector = sector_at(view_x, view_y);
    if (sector >= 0) {
        const struct doom_level *lv = doom_level();

        view_floor = lv->sectors[sector].floor_height;
        view_z = view_floor + DOOM3D_EYE_HEIGHT;
    }
}

void doom3d_walk(int32_t distance)
{
    int32_t sin_a;
    int32_t cos_a;

    if (distance == 0) {
        return;
    }
    rotate(view_angle * 65536 / 360, &sin_a, &cos_a);
    doom3d_step((distance * cos_a) >> 16, (distance * sin_a) >> 16);
}

void doom3d_get_view(int32_t *x, int32_t *y, int32_t *angle, int32_t *floor)
{
    if (x != 0) {
        *x = view_x;
    }
    if (y != 0) {
        *y = view_y;
    }
    if (angle != 0) {
        *angle = view_angle;
    }
    if (floor != 0) {
        *floor = view_floor;
    }
}

uint16_t doom3d_solid_count(void)
{
    return solid_count;
}

int doom3d_subsector_sector(uint16_t index)
{
    const struct doom_level *lv = doom_level();
    const struct doom_subsector *ss;
    uint16_t first;

    if (lv == 0 || index >= lv->subsector_count) {
        return -1;
    }
    ss = &lv->subsectors[index];
    if (ss->count == 0U) {
        return -1;
    }
    /* Doom takes a subsector's sector from its first seg, and every seg in a
     * subsector belongs to that one sector, so this is not a choice. */
    first = ss->first;
    if (first >= lv->seg_count) {
        return -1;
    }
    return seg_sector(lv, &lv->segs[first]);
}

const char *doom3d_error(void)
{
    return error_text;
}

/* -------------------------------------------------------------- drawing -- */

/* A fixed hue per sector, so rooms are as distinguishable as they would be by
 * texture.  `level` is the sector's light level already scaled to 0..1024 by
 * the caller, which is what lets a wall, a floor and a ceiling of the same
 * sector come out at three different brightnesses instead of all clamping to
 * full.  `dist` is in map units.
 *
 * The distance fade has to be steep.  Doom sectors are tens of units across, so
 * a gentle falloff leaves a whole room at nearly the same brightness and the
 * view comes out as one flat block of colour. */
static uint32_t sector_color(uint16_t sector, int32_t level, int32_t dist)
{
    static const uint16_t hues[8] = { 0, 40, 80, 120, 160, 200, 240, 20 };
    uint32_t hue = hues[sector & 7U];
    int32_t shade;
    int32_t wedge;
    int32_t f;
    int32_t t1;
    int32_t c[3];
    int32_t r;
    int32_t g;
    int32_t b;

    if (level < 0) {
        level = 0;
    }
    if (level > 1024) {
        level = 1024;
    }
    shade = 1024 - ((dist * 3) / 4);
    if (shade < 64) {
        shade = 64;
    }
    shade = (level * shade) / 1024;

    /* Six 60 degree wedges, blended, so no trigonometry is needed. */
    wedge = (int32_t)(hue / 60U) % 6;
    f = (int32_t)(hue % 60U) * 255 / 60;
    t1 = 255 - f;
    switch (wedge) {
    case 0: c[0] = 255; c[1] = f;   c[2] = 0;   break;
    case 1: c[0] = t1;  c[1] = 255; c[2] = 0;   break;
    case 2: c[0] = 0;   c[1] = 255; c[2] = f;   break;
    case 3: c[0] = 0;   c[1] = t1;  c[2] = 255; break;
    case 4: c[0] = f;   c[1] = 0;   c[2] = 255; break;
    default: c[0] = 255; c[1] = 0;   c[2] = t1;  break;
    }
    r = (c[0] * shade) >> 10;
    g = (c[1] * shade) >> 10;
    b = (c[2] * shade) >> 10;
    return 0xFF000000U | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void column(struct gfx_surface *s, int32_t ox, int32_t x, int32_t y0,
                   int32_t y1, uint32_t color, uint32_t *written)
{
    if (y0 < 0) {
        y0 = 0;
    }
    if (y1 > (int32_t)s->height) {
        y1 = (int32_t)s->height;
    }
    x += ox;
    if (y1 <= y0) {
        return;
    }
    {
        int32_t y;

        for (y = y0; y < y1; y++) {
            s->pixels[(uint32_t)y * s->pitch_pixels + (uint32_t)x] = color;
        }
    }
    *written += (uint32_t)(y1 - y0);
}

bool doom3d_render(struct gfx_surface *surface, int origin_x, int origin_y,
                   int width, int height, uint32_t void_color,
                   struct doom3d_stats *out)
{
    const struct doom_level *lv = doom_level();
    int32_t sin_a;
    int32_t cos_a;
    int32_t focal;
    int32_t cx;
    int32_t cy;
    int32_t view_w;
    int32_t view_h;
    int32_t sx;
    uint32_t written = 0;
    uint16_t hit_columns = 0;
    uint16_t open_columns = 0;
    uint32_t y;

    if (out != 0) {
        __builtin_memset(out, 0, sizeof(*out));
    }
    if (prepared == false) {
        fail("geometry not prepared");
        return false;
    }
    if (surface == 0 || width <= 0 || height <= 0) {
        fail("no surface");
        return false;
    }
    if (origin_x < 0 || origin_y < 0 ||
        origin_x + width > (int)surface->width ||
        origin_y + height > (int)surface->height) {
        fail("view does not fit the surface");
        return false;
    }
    if (lv == 0) {
        fail("no level is loaded");
        return false;
    }

    for (y = 0; y < (uint32_t)height; y++) {
        uint32_t *row = surface->pixels +
                        ((uint32_t)(origin_y + y) * surface->pitch_pixels) +
                        (uint32_t)origin_x;
        uint32_t x;

        for (x = 0; x < (uint32_t)width; x++) {
            row[x] = void_color;
        }
    }

    rotate(view_angle * 65536 / 360, &sin_a, &cos_a);
    view_w = width;
    view_h = height;
    focal = width / 2; /* a 90 degree horizontal field of view */
    cx = origin_x + width / 2;
    cy = origin_y + height / 2;

    for (sx = 0; sx < view_w; sx++) {
        /* Ray direction, not normalised: forward plus the screen offset
         * sideways.  The two are orthogonal unit vectors in Q16, so the length
         * falls out of the shift below without any square root. */
        int32_t u = (sx - cx) * 65536 / (focal > 0 ? focal : 1);
        int32_t dx = cos_a + (int32_t)((-(int64_t)sin_a * u) >> 16);
        int32_t dy = sin_a + (int32_t)(((int64_t)cos_a * u) >> 16);
        int64_t best_t = 0;
        const struct seg *best = 0;
        int64_t near_t = 0;
        const struct seg *near_any = 0;
        uint16_t i;
        int32_t top;
        int32_t bottom;
        int32_t dist;

        for (i = 0; i < seg_count; i++) {
            const struct seg *s = &segs[i];
            int32_t ax = s->x1 - view_x;
            int32_t ay = s->y1 - view_y;
            int32_t ex = s->x2 - s->x1;
            int32_t ey = s->y2 - s->y1;
            int64_t denom = (int64_t)dx * ey - (int64_t)dy * ex;
            int64_t numer;
            int64_t along;
            int64_t across;

            if (denom == 0) {
                continue;
            }
            numer = (int64_t)ax * ey - (int64_t)ay * ex;
            if (numer == 0) {
                continue;
            }
            /* The direction is in Q16, so the hit distance has to be scaled
             * back out or it truncates to zero for anything nearer than a
             * whole unit of the direction vector, which is every wall in the
             * room.  Same for the across test, which is a plain ratio. */
            along = (numer << 16) / denom;
            if (along <= DOOM3D_NEAR) {
                continue;
            }
            across = (((int64_t)ax * dy - (int64_t)ay * dx) << 16) / denom;
            if (across < 0 || across > 65536) {
                continue; /* the hit is past the end of the seg */
            }
            if (near_any == 0 || along < near_t) {
                near_t = along;
                near_any = s;
            }
            if (s->solid && (best == 0 || along < best_t)) {
                best_t = along;
                best = s;
            }
        }

        /* A doorway does not stop the ray, so when nothing solid is hit the
         * nearest crossing still gives a sector to draw the floor and ceiling
         * of.  Without that an open area would show as void. */
        if (best == 0) {
            best = near_any;
        }
        if (best == 0) {
            open_columns++;
            continue;
        }
        hit_columns++;

        dist = (int32_t)best_t; /* already map units */
        if (dist < 1) {
            dist = 1;
        }
        /* screen y = centre - focal * (height - eye) / distance */
        top = cy - (int32_t)(((int64_t)focal * (best->ceiling - view_z)) / dist);
        bottom = cy - (int32_t)(((int64_t)focal * (best->floor - view_z)) / dist);
        if (top < 0) {
            top = 0;
        }
        if (bottom > (int32_t)surface->height) {
            bottom = (int32_t)surface->height;
        }
        /* Fill the whole column: the wall between its base and its top, then
         * the floor running back from the base and the ceiling running back
         * from the top.  Those two bands are what a ray cast cannot produce by
         * itself, because a plane is not a hit, and leaving them empty is what
         * made the first version of this look like floating walls. */
        {
            int32_t light = (int32_t)lv->sectors[best->front].light * 4U;

            if (light > 1024) {
                light = 1024;
            }
            /* top and bottom come out of the projection in absolute surface
             * rows, because the centre row is origin_y plus half the height.
             * Adding the origin again here overshoots the window by exactly
             * its own y, which is invisible when the origin is zero. */
            if (top < origin_y) {
                top = origin_y;
            }
            if (bottom > origin_y + view_h) {
                bottom = origin_y + view_h;
            }
            column(surface, origin_x, sx, top, bottom,
                   sector_color(best->front, light, dist), &written);
            if (bottom < origin_y + view_h) {
                column(surface, origin_x, sx, bottom, origin_y + view_h,
                       sector_color(best->front, light / 2, dist), &written);
            }
            if (top > origin_y) {
                column(surface, origin_x, sx, origin_y, top,
                       sector_color(best->front, light / 4, dist), &written);
            }
        }
    }

    if (out != 0) {
        out->segs = seg_count;
        out->solid = solid_count;
        out->columns = hit_columns;
        out->open = open_columns;
        out->pixels = written;
    }
    return true;
}
