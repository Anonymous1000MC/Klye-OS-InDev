#ifndef KLYE_GFX_H
#define KLYE_GFX_H

#include <stdbool.h>
#include <stdint.h>

struct gfx_surface {
    uint32_t *pixels;
    uint32_t width;
    uint32_t height;
    uint32_t pitch_pixels;
    int origin_x;
    int origin_y;
};

struct gfx_rect {
    int x;
    int y;
    int width;
    int height;
};

void gfx_init(uint32_t framebuffer, uint32_t pitch_bytes, uint32_t width,
              uint32_t height);
struct gfx_surface *gfx_backbuffer(void);
struct gfx_surface *gfx_display(void);
uint32_t gfx_width(void);
uint32_t gfx_height(void);
uint32_t gfx_stride(void);

void gfx_damage(int x, int y, int width, int height);
void gfx_damage_all(void);
bool gfx_has_damage(void);
bool gfx_damage_region(struct gfx_rect *out);
void gfx_present(void);
void gfx_clear_damage(void);
uint32_t gfx_present_count(void);
uint32_t gfx_last_present_rows(void);

void gfx_fill(struct gfx_surface *surface, int x, int y, int width,
              int height, uint32_t color);
void gfx_blend_rect(struct gfx_surface *surface, int x, int y, int width,
                    int height, uint32_t color, uint32_t alpha);
void gfx_blit(struct gfx_surface *destination, int x, int y, int width,
              int height, const struct gfx_surface *source, int source_x,
              int source_y);
void gfx_blit_alpha(struct gfx_surface *destination, int x, int y, int width,
                    int height, const struct gfx_surface *source,
                    int source_x, int source_y, uint32_t alpha);
/* Damage clipping.  gfx_set_clip narrows every subsequent drawing call to the
 * rectangle; gfx_clip_none restores it.  The primitives intersect it internally,
 * so nothing has to be told about it twice. */
void gfx_set_clip(int x, int y, int width, int height);
void gfx_clip_none(void);
bool gfx_row_visible(int top, int bottom);

void gfx_pixel(struct gfx_surface *surface, int x, int y, uint32_t color);
void gfx_blend_pixel(struct gfx_surface *surface, int x, int y,
                     uint32_t color, uint32_t alpha);
uint32_t gfx_get_pixel(const struct gfx_surface *surface, int x, int y);

void gfx_rounded_rect(struct gfx_surface *surface, int x, int y, int width,
                      int height, int radius, uint32_t color);
void gfx_rounded_rect_alpha(struct gfx_surface *surface, int x, int y,
                            int width, int height, int radius,
                            uint32_t color, uint32_t alpha);
void gfx_rounded_border(struct gfx_surface *surface, int x, int y, int width,
                        int height, int radius, int thickness,
                        uint32_t color);
void gfx_rounded_border_alpha(struct gfx_surface *surface, int x, int y,
                              int width, int height, int radius, int thickness,
                              uint32_t color, uint32_t alpha);
void gfx_rounded_shadow(struct gfx_surface *surface, int x, int y, int width,
                        int height, int radius, int spread, int layers,
                        uint32_t color, uint32_t peak_alpha);
void gfx_horizontal_line(struct gfx_surface *surface, int x, int y, int width,
                         uint32_t color);
void gfx_vertical_line(struct gfx_surface *surface, int x, int y, int height,
                       uint32_t color);
void gfx_circle(struct gfx_surface *surface, int center_x, int center_y,
                int radius, uint32_t color);
void gfx_ring(struct gfx_surface *surface, int center_x, int center_y,
              int radius, int thickness, uint32_t color);

void gfx_gradient_v(struct gfx_surface *surface, int x, int y, int width,
                    int height, uint32_t top, uint32_t bottom);
void gfx_box_blur(struct gfx_surface *surface, int x, int y, int width,
                  int height, int radius, uint32_t passes);
void gfx_tint_surface(struct gfx_surface *surface, int x, int y, int width,
                      int height, uint32_t tint, uint32_t strength);
void gfx_vignette(struct gfx_surface *surface, int x, int y, int width,
                  int height, uint32_t strength);
uint32_t gfx_blend(uint32_t background, uint32_t foreground, uint32_t alpha);
uint32_t gfx_mix(uint32_t a, uint32_t b, uint32_t amount);
uint32_t gfx_lerp_channel(uint32_t from, uint32_t to, uint32_t amount);
void gfx_desaturate(uint32_t color, uint32_t strength, uint32_t *out);
uint32_t gfx_shade(uint32_t color, int amount);
uint32_t gfx_alpha_of(uint32_t color);
uint32_t gfx_tint(uint32_t color, uint32_t tint_color, uint32_t strength);

void *gfx_alloc(uint32_t bytes);
void *gfx_frame_alloc(uint32_t bytes);
void gfx_frame_reset(void);
uint32_t gfx_alloc_bytes_used(void);
uint32_t gfx_frame_bytes_used(void);
uint32_t gfx_frame_bytes_free(void);

int gfx_clampi(int value, int minimum, int maximum);
int gfx_rect_clamp(struct gfx_rect *rect, int width, int height);
bool gfx_rect_empty(const struct gfx_rect *rect);
bool gfx_rect_overlaps(const struct gfx_rect *a, const struct gfx_rect *b);
bool gfx_rect_contains(const struct gfx_rect *rect, int x, int y);
void gfx_rect_union(struct gfx_rect *a, const struct gfx_rect *b);

#endif
