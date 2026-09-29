#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gfx.h"
#include "heap.h"
#include "theme.h"
#include "kernel.h"

#define GFX_MAX_RADIUS 48
#define GFX_ARENA_BYTES (12U * 1024U * 1024U)
#define GFX_FRAME_RESERVE_BYTES (2U * 1024U * 1024U)
#define GFX_BLUR_ROW_MAX 2048U
#define GFX_BLUR_COLUMN_MAX 512U

typedef uint64_t gfx_word __attribute__((__may_alias__));
typedef uint32_t gfx_dword __attribute__((__may_alias__));

struct gfx_rect damage_rect;
static bool damage_pending;

static struct gfx_surface back_surface;
static struct gfx_surface display_surface;
static uint32_t *back_pixels;
static uint32_t present_counter;
static uint32_t present_rows;
/* The arena used to be a 12 MiB static array, which was almost the whole BSS
 * budget and left nothing for the frame heap or for future work.  It is now
 * taken from the Multiboot-backed frame heap, which has hundreds of MiB. */
static uint8_t *gfx_arena;
static size_t arena_bytes;


static uint32_t blur_rows[GFX_BLUR_ROW_MAX];
static uint32_t blur_columns[GFX_BLUR_COLUMN_MAX];

static uint8_t *arena_base;
static uint32_t *pool_base;
static uint32_t *pool_cursor;
static uint32_t *frame_pool_end;
static uint32_t pool_limit;
static uint32_t *frame_pool_cursor;

static uint8_t radius_inset[GFX_MAX_RADIUS + 1][GFX_MAX_RADIUS + 1];
static uint8_t radius_alpha[GFX_MAX_RADIUS + 1][GFX_MAX_RADIUS + 1];
static bool radius_ready;

static bool surface_writable(const struct gfx_surface *surface)
{
    return surface != NULL && surface->pixels != NULL;
}

int gfx_clampi(int value, int minimum, int maximum)
{
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

static uint32_t div255(uint32_t value)
{
    uint32_t biased = value + 128U;

    return (biased + (biased >> 8)) >> 8;
}

uint32_t gfx_blend(uint32_t background, uint32_t foreground, uint32_t alpha)
{
    uint32_t weight = (alpha > 255U) ? 255U : alpha;
    uint32_t inverse = 255U - weight;
    uint32_t red;
    uint32_t green;
    uint32_t blue;

    /* `alpha` is how much of the foreground to show: 255 is the foreground on
     * its own, 0 leaves the background alone.
     *
     * It used to be the other way round, weighting the background by alpha and
     * the foreground by what was left over.  The name says opacity, every
     * caller in the tree assumed it, and PNG stores its alpha channel the same
     * way, so a fully transparent pixel came out as a fully opaque one and a
     * caller asking for "mostly this colour" got mostly the old colour.  Two
     * places had noticed and written 255U - strength to compensate, which is
     * what made the convention look deliberate from a distance. */
    red = div255((((foreground >> 16) & 0xFFU) * weight) +
                 (((background >> 16) & 0xFFU) * inverse));
    green = div255((((foreground >> 8) & 0xFFU) * weight) +
                   (((background >> 8) & 0xFFU) * inverse));
    blue = div255(((foreground & 0xFFU) * weight) +
                  ((background & 0xFFU) * inverse));

    return (red << 16) | (green << 8) | blue;
}

uint32_t gfx_mix(uint32_t a, uint32_t b, uint32_t amount)
{
    return gfx_blend(a, b, amount);
}

uint32_t gfx_lerp_channel(uint32_t from, uint32_t to, uint32_t amount)
{
    uint32_t weight = (amount > 255U) ? 255U : amount;
    uint32_t inverse = 255U - weight;

    return div255((from * inverse) + (to * weight));
}

uint32_t gfx_shade(uint32_t color, int amount)
{
    uint32_t red = gfx_clampi((int)((color >> 16) & 0xFFU) + amount, 0, 255);
    uint32_t green = gfx_clampi((int)((color >> 8) & 0xFFU) + amount, 0, 255);
    uint32_t blue = gfx_clampi((int)(color & 0xFFU) + amount, 0, 255);

    return (red << 16) | (green << 8) | blue;
}

void gfx_desaturate(uint32_t color, uint32_t strength, uint32_t *out)
{
    uint32_t red = (color >> 16) & 0xFFU;
    uint32_t green = (color >> 8) & 0xFFU;
    uint32_t blue = color & 0xFFU;
    uint32_t luma = (red * 77U + green * 151U + blue * 28U) >> 8;
    uint32_t weight = (strength > 255U) ? 255U : strength;
    uint32_t inverse = 255U - weight;

    red = div255((red * inverse) + (luma * weight));
    green = div255((green * inverse) + (luma * weight));
    blue = div255((blue * inverse) + (luma * weight));
    *out = (red << 16) | (green << 8) | blue;
}

uint32_t gfx_tint(uint32_t color, uint32_t tint_color, uint32_t strength)
{
    uint32_t desaturated;
    uint32_t result;

    gfx_desaturate(color, strength, &desaturated);
    result = gfx_blend(desaturated, tint_color, strength);
    return result;
}

uint32_t gfx_alpha_of(uint32_t color)
{
    return (color >> 24) & 0xFFU;
}

static uint32_t isqrt32(uint32_t value)
{
    uint32_t remainder = value;
    uint32_t result = 0;
    uint32_t bit = 1U << 30;

    while (bit > remainder) {
        bit >>= 2;
    }
    while (bit != 0U) {
        if (remainder >= result + bit) {
            remainder -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return result;
}

static void build_radius_tables(void)
{
    if (radius_ready) {
        return;
    }
    for (int radius = 0; radius <= GFX_MAX_RADIUS; ++radius) {
        for (int row = 0; row < radius; ++row) {
            int32_t f = (int32_t)(2 * row + 1 - 2 * radius) * 128;
            int64_t s2 = (int64_t)radius * radius * 65536 -
                         (int64_t)f * f;
            uint32_t s;
            int64_t edge;
            int32_t full;
            int64_t cover;

            if (s2 < 0) {
                s2 = 0;
            }
            s = isqrt32((uint32_t)s2);
            edge = (int64_t)radius * 65536 - 32768 - (int64_t)s;
            full = (int32_t)((edge - 32768) >> 16);
            if (full < -1) {
                full = -1;
            }
            cover = edge - (int64_t)(full + 1) * 65536;
            if (cover < 0) {
                cover = 0;
            }
            if (cover > 65536) {
                cover = 65536;
            }
            radius_inset[radius][row] = (uint8_t)(full + 1);
            radius_alpha[radius][row] =
                (uint8_t)((cover >= 65536) ? 255 : (cover >> 8));
        }
        for (int row = radius; row <= GFX_MAX_RADIUS; ++row) {
            radius_inset[radius][row] = 0;
            radius_alpha[radius][row] = 255;
        }
    }
    radius_ready = true;
}

static uint32_t align_up_u32(uint32_t value, uint32_t alignment)
{
    return (value + (alignment - 1U)) & ~(alignment - 1U);
}

void *gfx_alloc(uint32_t bytes)
{
    uint32_t aligned = align_up_u32(bytes, 16U);
    uint32_t words = aligned / 4U;
    void *result;

    if (pool_cursor == NULL || words == 0U) {
        return NULL;
    }
    if ((uint32_t)(frame_pool_cursor - pool_cursor) < words ||
        (uint32_t)((uint8_t *)pool_cursor - arena_base) + aligned > pool_limit) {
        return NULL;
    }
    result = pool_cursor;
    pool_cursor += words;
    return result;
}

void *gfx_frame_alloc(uint32_t bytes)
{
    uint32_t aligned = align_up_u32(bytes, 16U);
    uint32_t words = aligned / 4U;
    void *result;

    if (frame_pool_cursor == NULL || words == 0U) {
        return NULL;
    }
    if ((uint32_t)(frame_pool_cursor - pool_cursor) < words) {
        return NULL;
    }
    result = frame_pool_cursor - words;
    frame_pool_cursor = result;
    return result;
}

void gfx_frame_reset(void)
{
    frame_pool_cursor = frame_pool_end;
}

uint32_t gfx_alloc_bytes_used(void)
{
    if (pool_cursor == NULL) {
        return 0U;
    }
    return (uint32_t)((uint8_t *)pool_cursor - arena_base);
}

uint32_t gfx_frame_bytes_used(void)
{
    if (frame_pool_end == NULL || frame_pool_cursor == NULL) {
        return 0U;
    }
    return (uint32_t)((uint8_t *)frame_pool_end - (uint8_t *)frame_pool_cursor);
}

uint32_t gfx_frame_bytes_free(void)
{
    if (frame_pool_cursor == NULL || pool_cursor == NULL) {
        return 0U;
    }
    return (uint32_t)((uint8_t *)frame_pool_cursor - (uint8_t *)pool_cursor);
}

void gfx_init(uint32_t framebuffer, uint32_t pitch_bytes, uint32_t width,
              uint32_t height)
{
    uint32_t pixels;

    display_surface.pixels = (uint32_t *)(uintptr_t)framebuffer;
    display_surface.width = width;
    display_surface.height = height;
    display_surface.pitch_pixels = pitch_bytes / 4U;
    display_surface.origin_x = 0;
    display_surface.origin_y = 0;

    pixels = width * height;
    if (gfx_arena == 0) {
        /* Ask for the full arena first, then fall back to just enough for the
         * back buffer.  A machine too small for either is not going to run
         * this desktop, and saying so plainly beats a corrupt display. */
        size_t minimum = (size_t)pixels * 4U + GFX_FRAME_RESERVE_BYTES;
        void *pages = heap_alloc_pages(GFX_ARENA_BYTES);

        if (pages == 0) {
            pages = heap_alloc_pages(minimum);
            if (pages == 0) {
                panic("gfx: not enough memory for the framebuffer");
            }
            arena_bytes = minimum;
        } else {
            arena_bytes = GFX_ARENA_BYTES;
        }
        gfx_arena = (uint8_t *)(uintptr_t)pages;
    }
    arena_base = gfx_arena;
    pool_base = (uint32_t *)(void *)gfx_arena;
    pool_cursor = pool_base;
    pool_limit = arena_bytes - GFX_FRAME_RESERVE_BYTES;
    frame_pool_end = (uint32_t *)(void *)(gfx_arena + pool_limit);
    frame_pool_cursor = frame_pool_end;
    (void)frame_pool_end;

    back_pixels = (uint32_t *)gfx_alloc(pixels * 4U);
    back_surface.pixels = back_pixels;
    if (back_pixels == NULL) {
        panic("gfx: back buffer allocation failed");
    }
    back_surface.width = width;
    back_surface.height = height;
    back_surface.pitch_pixels = width;
    back_surface.origin_x = 0;
    back_surface.origin_y = 0;

    build_radius_tables();
    damage_pending = true;
    gfx_damage_all();
}

struct gfx_surface *gfx_backbuffer(void)
{
    return &back_surface;
}

struct gfx_surface *gfx_display(void)
{
    return &display_surface;
}

uint32_t gfx_width(void)
{
    return back_surface.width;
}

uint32_t gfx_height(void)
{
    return back_surface.height;
}

uint32_t gfx_stride(void)
{
    return back_surface.pitch_pixels;
}

void gfx_damage(int x, int y, int width, int height)
{
    struct gfx_rect incoming;

    if (width <= 0 || height <= 0) {
        return;
    }
    incoming.x = x;
    incoming.y = y;
    incoming.width = width;
    incoming.height = height;
    if (gfx_rect_clamp(&incoming, (int)back_surface.width,
                       (int)back_surface.height)) {
        return;
    }
    if (!damage_pending) {
        damage_rect = incoming;
        damage_pending = true;
        return;
    }
    gfx_rect_union(&damage_rect, &incoming);
}

void gfx_damage_all(void)
{
    damage_rect.x = 0;
    damage_rect.y = 0;
    damage_rect.width = (int)back_surface.width;
    damage_rect.height = (int)back_surface.height;
    damage_pending = true;
}

bool gfx_has_damage(void)
{
    return damage_pending;
}

bool gfx_damage_region(struct gfx_rect *out)
{
    if (!damage_pending || out == NULL) {
        return false;
    }
    *out = damage_rect;
    return true;
}

void gfx_clear_damage(void)
{
    damage_pending = false;
}

uint32_t gfx_present_count(void)
{
    return present_counter;
}

uint32_t gfx_last_present_rows(void)
{
    return present_rows;
}

void gfx_present(void)
{
    struct gfx_rect region;
    uint32_t *source;
    uint32_t *destination;
    uint32_t rows;

    if (!damage_pending) {
        return;
    }
    region = damage_rect;
    if (gfx_rect_clamp(&region, (int)back_surface.width,
                       (int)back_surface.height)) {
        damage_pending = false;
        return;
    }
    source = back_surface.pixels +
             (uint32_t)region.y * back_surface.pitch_pixels +
             (uint32_t)region.x;
    destination = display_surface.pixels +
                  (uint32_t)region.y * display_surface.pitch_pixels +
                  (uint32_t)region.x;
    rows = (uint32_t)region.height;
    for (uint32_t row = 0; row < rows; ++row) {
        __builtin_memcpy(destination, source, (size_t)region.width * 4U);
        source += back_surface.pitch_pixels;
        destination += display_surface.pitch_pixels;
    }
    present_counter++;
    present_rows = rows;
    damage_pending = false;
}

void gfx_fill(struct gfx_surface *surface, int x, int y, int width, int height,
              uint32_t color)
{
    if (!surface_writable(surface)) {
        return;
    }

    int x_end;
    int y_end;
    gfx_word packed;
    gfx_word *wide;
    uint32_t count;

    if (width <= 0 || height <= 0) {
        return;
    }
    x_end = x + width;
    y_end = y + height;
    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }
    if (x_end > (int)surface->width) {
        x_end = (int)surface->width;
    }
    if (y_end > (int)surface->height) {
        y_end = (int)surface->height;
    }
    if (x >= x_end || y >= y_end) {
        return;
    }
    packed = ((gfx_word)color << 32) | (gfx_word)color;
    for (int row = y; row < y_end; ++row) {
        gfx_dword *line = surface->pixels +
                          (uint32_t)row * surface->pitch_pixels;
        int offset = x;

        count = (uint32_t)(x_end - x);
        wide = (gfx_word *)(void *)(line + offset);
        while (count >= 2U) {
            *wide++ = packed;
            count -= 2U;
        }
        if (count != 0U) {
            line[offset + (int)(x_end - x) - 1] = color;
        }
    }
}

void gfx_pixel(struct gfx_surface *surface, int x, int y, uint32_t color)
{
    if (!surface_writable(surface)) {
        return;
    }

    if (x < 0 || y < 0 || x >= (int)surface->width || y >= (int)surface->height) {
        return;
    }
    surface->pixels[(uint32_t)y * surface->pitch_pixels + (uint32_t)x] = color;
}

uint32_t gfx_get_pixel(const struct gfx_surface *surface, int x, int y)
{
    if (x < 0 || y < 0 || x >= (int)surface->width || y >= (int)surface->height) {
        return 0U;
    }
    if (!surface_writable(surface)) {
        return 0U;
    }
    return surface->pixels[(uint32_t)y * surface->pitch_pixels + (uint32_t)x];
}

void gfx_blend_pixel(struct gfx_surface *surface, int x, int y, uint32_t color,
                     uint32_t alpha)
{
    if (!surface_writable(surface)) {
        return;
    }

    uint32_t existing;

    if (alpha == 0U || x < 0 || y < 0 || x >= (int)surface->width ||
        y >= (int)surface->height) {
        return;
    }
    if (alpha >= 255U) {
        surface->pixels[(uint32_t)y * surface->pitch_pixels + (uint32_t)x] =
            color & 0x00FFFFFFU;
        return;
    }
    existing = surface->pixels[(uint32_t)y * surface->pitch_pixels +
                               (uint32_t)x];
    surface->pixels[(uint32_t)y * surface->pitch_pixels + (uint32_t)x] =
        gfx_blend(existing, color, alpha) & 0x00FFFFFFU;
}

void gfx_blend_rect(struct gfx_surface *surface, int x, int y, int width,
                    int height, uint32_t color, uint32_t alpha)
{
    if (!surface_writable(surface)) {
        return;
    }

    int x_end;
    int y_end;

    if (alpha == 0U || width <= 0 || height <= 0) {
        return;
    }
    x_end = x + width;
    y_end = y + height;
    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }
    if (x_end > (int)surface->width) {
        x_end = (int)surface->width;
    }
    if (y_end > (int)surface->height) {
        y_end = (int)surface->height;
    }
    if (x >= x_end || y >= y_end) {
        return;
    }
    if (alpha >= 255U) {
        gfx_fill(surface, x, y, x_end - x, y_end - y, color & 0x00FFFFFFU);
        return;
    }
    for (int row = y; row < y_end; ++row) {
        gfx_dword *line = surface->pixels +
                          (uint32_t)row * surface->pitch_pixels;

        for (int column = x; column < x_end; ++column) {
            line[column] = gfx_blend(line[column], color, alpha) & 0x00FFFFFFU;
        }
    }
}

void gfx_blit(struct gfx_surface *destination, int x, int y, int width,
              int height, const struct gfx_surface *source, int source_x,
              int source_y)
{
    if (!surface_writable(destination)) {
        return;
    }

    int x_end;
    int y_end;

    if (width <= 0 || height <= 0) {
        return;
    }
    x_end = x + width;
    y_end = y + height;
    if (x < 0) {
        source_x += -x;
        width += x;
        x = 0;
    }
    if (y < 0) {
        source_y += -y;
        height += y;
        y = 0;
    }
    if (x_end > (int)destination->width) {
        x_end = (int)destination->width;
    }
    if (y_end > (int)destination->height) {
        y_end = (int)destination->height;
    }
    if (x >= x_end || y >= y_end) {
        return;
    }
    if (source_x < 0) {
        x += -source_x;
        width += source_x;
        source_x = 0;
    }
    if (source_y < 0) {
        y += -source_y;
        height += source_y;
        source_y = 0;
    }
    if (x_end - x > (int)source->width - source_x) {
        x_end = x + (int)source->width - source_x;
    }
    if (y_end - y > (int)source->height - source_y) {
        y_end = y + (int)source->height - source_y;
    }
    if (x >= x_end || y >= y_end) {
        return;
    }
    for (int row = y; row < y_end; ++row) {
        const uint32_t *src = source->pixels +
                              (uint32_t)(row - y + source_y) *
                                  source->pitch_pixels +
                              (uint32_t)source_x;
        uint32_t *dst = destination->pixels +
                        (uint32_t)row * destination->pitch_pixels + (uint32_t)x;

        __builtin_memcpy(dst, src, (size_t)(x_end - x) * 4U);
    }
}

void gfx_blit_alpha(struct gfx_surface *destination, int x, int y, int width,
                    int height, const struct gfx_surface *source,
                    int source_x, int source_y, uint32_t alpha)
{
    if (!surface_writable(destination)) {
        return;
    }

    if (alpha == 0U || width <= 0 || height <= 0) {
        return;
    }
    for (int row = 0; row < height; ++row) {
        int dest_y = y + row;
        const uint32_t *src;
        uint32_t *dst;

        if (dest_y < 0 || dest_y >= (int)destination->height) {
            continue;
        }
        if (source_y + row < 0 || source_y + row >= (int)source->height) {
            continue;
        }
        src = source->pixels +
              (uint32_t)(source_y + row) * source->pitch_pixels;
        dst = destination->pixels +
              (uint32_t)dest_y * destination->pitch_pixels;
        for (int column = 0; column < width; ++column) {
            int dest_x = x + column;
            int source_col = source_x + column;

            if (dest_x < 0 || dest_x >= (int)destination->width) {
                continue;
            }
            if (source_col < 0 || source_col >= (int)source->width) {
                continue;
            }
            dst[dest_x] = gfx_blend(dst[dest_x], src[source_col], alpha) &
                          0x00FFFFFFU;
        }
    }
}

static int clamp_radius(int radius, int width, int height)
{
    int limit = width < height ? width : height;

    if (radius > limit / 2) {
        radius = limit / 2;
    }
    if (radius > GFX_MAX_RADIUS) {
        radius = GFX_MAX_RADIUS;
    }
    if (radius < 0) {
        radius = 0;
    }
    return radius;
}

void gfx_rounded_rect_alpha(struct gfx_surface *surface, int x, int y, int width,
                            int height, int radius, uint32_t color,
                            uint32_t alpha)
{
    if (!surface_writable(surface)) {
        return;
    }

    int top;
    int bottom;
    int r;

    if (width <= 0 || height <= 0 || alpha == 0U) {
        return;
    }
    build_radius_tables();
    r = clamp_radius(radius, width, height);
    top = y < 0 ? 0 : y;
    bottom = y + height;
    if (bottom > (int)surface->height) {
        bottom = (int)surface->height;
    }
    for (int row = top; row < bottom; ++row) {
        int local = row - y;
        int distance_top = local;
        int distance_bottom = height - 1 - local;
        int edge_alpha = 255;
        int inset = 0;
        int span_start;
        int span_end;

        if (distance_bottom < distance_top) {
            distance_top = distance_bottom;
        }
        if (r > 0 && distance_top < r) {
            inset = radius_inset[r][distance_top];
            edge_alpha = radius_alpha[r][distance_top];
        }
        span_start = x + inset;
        span_end = x + width - inset;
        if (span_end <= span_start) {
            continue;
        }
        if (alpha >= 255U) {
            if (edge_alpha >= 255) {
                gfx_fill(surface, span_start, row, span_end - span_start, 1,
                         color);
            } else if (edge_alpha > 0) {
                gfx_fill(surface, span_start + 1, row,
                         span_end - span_start - 2, 1, color);
                gfx_blend_pixel(surface, span_start, row, color,
                                (uint32_t)edge_alpha);
                gfx_blend_pixel(surface, span_end - 1, row, color,
                                (uint32_t)edge_alpha);
            }
        } else {
            uint32_t row_alpha = alpha;
            gfx_dword *line = surface->pixels +
                              (uint32_t)row * surface->pitch_pixels;

            if (edge_alpha < 255) {
                row_alpha = (alpha * (uint32_t)edge_alpha) / 255U;
            }
            if (row_alpha == 0U) {
                continue;
            }
            for (int column = span_start; column < span_end; ++column) {
                line[column] = gfx_blend(line[column], color, row_alpha) &
                               0x00FFFFFFU;
            }
        }
    }
}

void gfx_rounded_rect(struct gfx_surface *surface, int x, int y, int width,
                      int height, int radius, uint32_t color)
{
    gfx_rounded_rect_alpha(surface, x, y, width, height, radius, color, 255U);
}

void gfx_rounded_border_alpha(struct gfx_surface *surface, int x, int y,
                              int width, int height, int radius, int thickness,
                              uint32_t color, uint32_t alpha)
{
    if (!surface_writable(surface)) {
        return;
    }

    int r;
    int inset;

    if (thickness <= 0 || width <= 0 || height <= 0) {
        return;
    }
    build_radius_tables();
    r = clamp_radius(radius, width, height);
    for (int t = 0; t < thickness; ++t) {
        int outer_x = x + t;
        int outer_y = y + t;
        int outer_w = width - t * 2;
        int outer_h = height - t * 2;

        if (outer_w <= 0 || outer_h <= 0) {
            break;
        }
        inset = r - t;
        if (inset < 0) {
            inset = 0;
        }
        for (int row = 0; row < outer_h; ++row) {
            int distance = row < inset ? inset - row : row;
            int other = outer_h - 1 - row;
            int edge_alpha = 255;
            int local_inset = 0;

            if (other < distance) {
                distance = other;
            }
            if (distance < 0) {
                distance = 0;
            }
            if (inset > 0 && distance < inset) {
                local_inset = radius_inset[inset][distance];
                edge_alpha = radius_alpha[inset][distance];
            }
            uint32_t row_alpha = div255((uint32_t)edge_alpha * alpha);

            gfx_blend_pixel(surface, outer_x + local_inset, outer_y + row,
                            color, row_alpha);
            gfx_blend_pixel(surface, outer_x + outer_w - local_inset - 1,
                            outer_y + row, color, row_alpha);
        }
        for (int column = 0; column < outer_w; ++column) {
            gfx_blend_pixel(surface, outer_x + column, outer_y, color, alpha);
            gfx_blend_pixel(surface, outer_x + column, outer_y + outer_h - 1,
                            color, alpha);
        }
    }
}

void gfx_rounded_border(struct gfx_surface *surface, int x, int y, int width,
                        int height, int radius, int thickness,
                        uint32_t color)
{
    gfx_rounded_border_alpha(surface, x, y, width, height, radius, thickness,
                             color, 255U);
}

void gfx_rounded_shadow(struct gfx_surface *surface, int x, int y, int width,
                        int height, int radius, int spread, int layers,
                        uint32_t color, uint32_t peak_alpha)
{
    if (layers <= 0 || spread <= 0) {
        return;
    }
    for (int layer = layers; layer >= 1; --layer) {
        int grow = (spread * layer) / layers;
        uint32_t alpha = (peak_alpha * (uint32_t)layer) / (uint32_t)layers;

        if (alpha == 0U) {
            continue;
        }
        gfx_rounded_rect_alpha(surface, x - grow, y - grow + grow / 3,
                               width + grow * 2, height + grow * 2,
                               radius + grow, color, alpha);
    }
}

void gfx_horizontal_line(struct gfx_surface *surface, int x, int y, int width,
                         uint32_t color)
{
    if (!surface_writable(surface)) {
        return;
    }

    gfx_fill(surface, x, y, width, 1, color);
}

void gfx_vertical_line(struct gfx_surface *surface, int x, int y, int height,
                       uint32_t color)
{
    if (!surface_writable(surface)) {
        return;
    }

    gfx_fill(surface, x, y, 1, height, color);
}

void gfx_circle(struct gfx_surface *surface, int center_x, int center_y,
                int radius, uint32_t color)
{
    if (!surface_writable(surface)) {
        return;
    }

    if (radius <= 0) {
        return;
    }
    for (int dy = -radius; dy <= radius; ++dy) {
        int span = isqrt32((uint32_t)((radius - dy) * (radius - dy)));
        gfx_fill(surface, center_x - span, center_y + dy, span * 2 + 1, 1,
                 color);
    }
}

void gfx_ring(struct gfx_surface *surface, int center_x, int center_y,
              int radius, int thickness, uint32_t color)
{
    if (!surface_writable(surface)) {
        return;
    }

    if (radius <= 0 || thickness <= 0) {
        return;
    }
    for (int dy = -radius; dy <= radius; ++dy) {
        int distance = dy < 0 ? -dy : dy;
        int outer = isqrt32((uint32_t)((radius - distance) *
                                       (radius - distance)));
        int inner_r = radius - thickness;
        int inner = 0;

        if (inner_r > 0) {
            int remaining = inner_r * inner_r - distance * distance;
            inner = remaining > 0 ? isqrt32((uint32_t)remaining) : 0;
        }
        if (outer > inner) {
            gfx_fill(surface, center_x - outer, center_y + dy,
                     (outer - inner) * 2 + 1, 1, color);
        }
    }
}

void gfx_gradient_v(struct gfx_surface *surface, int x, int y, int width,
                    int height, uint32_t top, uint32_t bottom)
{
    if (!surface_writable(surface)) {
        return;
    }

    if (width <= 0 || height <= 0) {
        return;
    }
    for (int row = 0; row < height; ++row) {
        uint32_t amount = (uint32_t)((row * 255) / (height > 1 ? height - 1 : 1));
        uint32_t color = PIXEL_RGB(gfx_lerp_channel((top >> 16) & 0xFFU,
                                                    (bottom >> 16) & 0xFFU, amount),
                                   gfx_lerp_channel((top >> 8) & 0xFFU,
                                                    (bottom >> 8) & 0xFFU, amount),
                                   gfx_lerp_channel(top & 0xFFU, bottom & 0xFFU,
                                                    amount));

        gfx_fill(surface, x, y + row, width, 1, color);
    }
}

static void blur_row(uint32_t *line, int count, int radius)
{
    uint32_t sum_r = 0;
    uint32_t sum_g = 0;
    uint32_t sum_b = 0;

    if (count > (int)GFX_BLUR_ROW_MAX) {
        return;
    }
    for (int column = -radius; column <= radius; ++column) {
        uint32_t pixel = line[gfx_clampi(column, 0, count - 1)];

        sum_r += (pixel >> 16) & 0xFFU;
        sum_g += (pixel >> 8) & 0xFFU;
        sum_b += pixel & 0xFFU;
    }
    for (int column = 0; column < count; ++column) {
        int leaving = gfx_clampi(column - radius, 0, count - 1);
        int entering = gfx_clampi(column + radius + 1, 0, count - 1);
        uint32_t out_pixel = line[leaving];
        uint32_t in_pixel = line[entering];
        uint32_t window = (uint32_t)radius * 2U + 1U;

        blur_rows[column] = PIXEL_RGB(div255(sum_r * window),
                                      div255(sum_g * window),
                                      div255(sum_b * window));
        sum_r -= (out_pixel >> 16) & 0xFFU;
        sum_g -= (out_pixel >> 8) & 0xFFU;
        sum_b -= out_pixel & 0xFFU;
        sum_r += (in_pixel >> 16) & 0xFFU;
        sum_g += (in_pixel >> 8) & 0xFFU;
        sum_b += in_pixel & 0xFFU;
    }
    for (int column = 0; column < count; ++column) {
        line[column] = blur_rows[column];
    }
}

static void blur_column(struct gfx_surface *surface, int x, int y, int height,
                        int column, int radius)
{
    uint32_t sum_r = 0;
    uint32_t sum_g = 0;
    uint32_t sum_b = 0;
    uint32_t window = (uint32_t)radius * 2U + 1U;

    if (height > (int)GFX_BLUR_COLUMN_MAX) {
        return;
    }
    for (int row = 0; row < height; ++row) {
        uint32_t pixel = surface->pixels[(uint32_t)(y + row) *
                                             surface->pitch_pixels +
                                         (uint32_t)(x + column)];

        blur_columns[row] = pixel;
        sum_r += (pixel >> 16) & 0xFFU;
        sum_g += (pixel >> 8) & 0xFFU;
        sum_b += pixel & 0xFFU;
    }
    for (int row = 0; row < height; ++row) {
        int leaving = gfx_clampi(row - radius, 0, height - 1);
        int entering = gfx_clampi(row + radius + 1, 0, height - 1);
        uint32_t out_pixel = blur_columns[leaving];
        uint32_t in_pixel = blur_columns[entering];
        uint32_t *target = surface->pixels +
                           (uint32_t)(y + row) * surface->pitch_pixels +
                           (uint32_t)(x + column);

        target[0] = PIXEL_RGB(div255(sum_r * window), div255(sum_g * window),
                             div255(sum_b * window));
        sum_r -= (out_pixel >> 16) & 0xFFU;
        sum_g -= (out_pixel >> 8) & 0xFFU;
        sum_b -= out_pixel & 0xFFU;
        sum_r += (in_pixel >> 16) & 0xFFU;
        sum_g += (in_pixel >> 8) & 0xFFU;
        sum_b += in_pixel & 0xFFU;
    }
}

void gfx_box_blur(struct gfx_surface *surface, int x, int y, int width,
                  int height, int radius, uint32_t passes)
{
    if (!surface_writable(surface)) {
        return;
    }

    struct gfx_rect region;

    region.x = x;
    region.y = y;
    region.width = width;
    region.height = height;
    if (gfx_rect_clamp(&region, (int)surface->width, (int)surface->height)) {
        return;
    }
    if (radius <= 0 || passes == 0U) {
        return;
    }
    if (region.width > (int)GFX_BLUR_ROW_MAX ||
        region.height > (int)GFX_BLUR_COLUMN_MAX) {
        return;
    }
    for (uint32_t pass = 0; pass < passes; ++pass) {
        for (int row = 0; row < region.height; ++row) {
            uint32_t *line = surface->pixels +
                             (uint32_t)(y + row) * surface->pitch_pixels +
                             (uint32_t)x;

            blur_row(line, region.width, radius);
        }
        for (int column = 0; column < region.width; ++column) {
            blur_column(surface, x, y, region.height, column, radius);
        }
    }
}

void gfx_tint_surface(struct gfx_surface *surface, int x, int y, int width,
                      int height, uint32_t tint, uint32_t strength)
{
    if (!surface_writable(surface)) {
        return;
    }

    for (int row = 0; row < height; ++row) {
        uint32_t *line = surface->pixels +
                          (uint32_t)(y + row) * surface->pitch_pixels;

        for (int column = 0; column < width; ++column) {
            int px = x + column;
            int py = y + row;
            uint32_t existing;

            if (px < 0 || py < 0 || px >= (int)surface->width ||
                py >= (int)surface->height) {
                continue;
            }
            existing = line[px];
            gfx_desaturate(existing, strength, &existing);
            line[px] = gfx_blend(existing, tint, strength) & 0x00FFFFFFU;
        }
    }
}

void gfx_vignette(struct gfx_surface *surface, int x, int y, int width,
                  int height, uint32_t strength)
{
    if (!surface_writable(surface)) {
        return;
    }

    int center_x = x + width / 2;
    int center_y = y + height / 2;
    int radius = isqrt32((uint32_t)((width / 2) * (width / 2) +
                                    (height / 2) * (height / 2)));

    if (radius <= 0) {
        return;
    }
    for (int row = 0; row < height; ++row) {
        uint32_t *line = surface->pixels +
                          (uint32_t)(y + row) * surface->pitch_pixels;

        for (int column = 0; column < width; ++column) {
            int dx = x + column - center_x;
            int dy = y + row - center_y;
            uint32_t distance = isqrt32((uint32_t)(dx * dx + dy * dy));
            uint32_t amount;

            if (distance <= (uint32_t)radius) {
                continue;
            }
            amount = ((distance - (uint32_t)radius) * strength) /
                     (uint32_t)radius;
            if (amount > 255U) {
                amount = 255U;
            }
            {
                int px = x + column;
                int py = y + row;

                if (px < 0 || py < 0 || px >= (int)surface->width ||
                    py >= (int)surface->height) {
                    continue;
                }
                /* 255 - amount, not amount: the tint is at its strongest in
                 * the far corner, where amount is largest, and the blend wants
                 * the corner to keep most of what is already there. */
                line[px] = gfx_blend(line[px], PIXEL_RGB(0x30, 0x38, 0x48),
                                     255U - amount);
            }
        }
    }
}

int gfx_rect_clamp(struct gfx_rect *rect, int width, int height)
{
    if (rect->width <= 0 || rect->height <= 0) {
        return 1;
    }
    if (rect->x < 0) {
        rect->width += rect->x;
        rect->x = 0;
    }
    if (rect->y < 0) {
        rect->height += rect->y;
        rect->y = 0;
    }
    if (rect->x + rect->width > width) {
        rect->width = width - rect->x;
    }
    if (rect->y + rect->height > height) {
        rect->height = height - rect->y;
    }
    return rect->width <= 0 || rect->height <= 0;
}

bool gfx_rect_empty(const struct gfx_rect *rect)
{
    return rect->width <= 0 || rect->height <= 0;
}

bool gfx_rect_overlaps(const struct gfx_rect *a, const struct gfx_rect *b)
{
    if (gfx_rect_empty(a) || gfx_rect_empty(b)) {
        return false;
    }
    if (a->x + a->width <= b->x || b->x + b->width <= a->x) {
        return false;
    }
    return a->y + a->height > b->y && b->y + b->height > a->y;
}

bool gfx_rect_contains(const struct gfx_rect *rect, int x, int y)
{
    if (gfx_rect_empty(rect)) {
        return false;
    }
    return x >= rect->x && x < rect->x + rect->width &&
           y >= rect->y && y < rect->y + rect->height;
}

void gfx_rect_union(struct gfx_rect *a, const struct gfx_rect *b)
{
    int x1;
    int y1;
    int x2;
    int y2;

    if (gfx_rect_empty(a)) {
        *a = *b;
        return;
    }
    if (gfx_rect_empty(b)) {
        return;
    }
    x1 = a->x < b->x ? a->x : b->x;
    y1 = a->y < b->y ? a->y : b->y;
    x2 = a->x + a->width;
    y2 = a->y + a->height;
    if (b->x + b->width > x2) {
        x2 = b->x + b->width;
    }
    if (b->y + b->height > y2) {
        y2 = b->y + b->height;
    }
    a->x = x1;
    a->y = y1;
    a->width = x2 - x1;
    a->height = y2 - y1;
}
