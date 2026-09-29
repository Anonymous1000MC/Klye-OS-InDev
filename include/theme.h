#ifndef KLYE_THEME_H
#define KLYE_THEME_H

#define PIXEL_ARGB(a, r, g, b) \
    (((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | \
     ((uint32_t)(g) << 8) | (uint32_t)(b))
#define PIXEL_RGB(r, g, b) PIXEL_ARGB(0xFF, r, g, b)
#define PIXEL_RGBA(r, g, b, a) PIXEL_ARGB(a, r, g, b)

#define COLOR_TRANSPARENT PIXEL_ARGB(0, 0, 0, 0)

#define THEME_TEXT_PRIMARY PIXEL_RGB(0x1D, 0x1D, 0x1F)
#define THEME_TEXT_SECONDARY PIXEL_RGB(0x5C, 0x5C, 0x62)
#define THEME_TEXT_TERTIARY PIXEL_RGB(0x8A, 0x8A, 0x90)
#define THEME_TEXT_ON_DARK PIXEL_RGB(0xF5, 0xF5, 0xF7)
#define THEME_TEXT_ON_DARK_DIM PIXEL_RGB(0xB4, 0xB4, 0xBC)

#define THEME_SURFACE PIXEL_RGB(0xFF, 0xFF, 0xFF)
#define THEME_SURFACE_RAISED PIXEL_RGB(0xFC, 0xFC, 0xFE)
#define THEME_SURFACE_SUNKEN PIXEL_RGB(0xF1, 0xF1, 0xF5)
#define THEME_SURFACE_HOVER PIXEL_RGB(0xE9, 0xE9, 0xEF)
#define THEME_SURFACE_ACTIVE PIXEL_RGB(0xDC, 0xDC, 0xE4)

#define THEME_SEPARATOR PIXEL_RGB(0xD9, 0xD9, 0xDF)
#define THEME_SEPARATOR_STRONG PIXEL_RGB(0xC2, 0xC2, 0xCB)

#define THEME_ACCENT PIXEL_RGB(0x0A, 0x84, 0xFF)
#define THEME_ACCENT_DEEP PIXEL_RGB(0x00, 0x60, 0xDF)
#define THEME_ACCENT_SOFT PIXEL_RGB(0xD6, 0xEB, 0xFF)

#define THEME_GREEN PIXEL_RGB(0x34, 0xC7, 0x59)
#define THEME_ORANGE PIXEL_RGB(0xFF, 0x9F, 0x0A)
#define THEME_RED PIXEL_RGB(0xFF, 0x3B, 0x30)
#define THEME_PURPLE PIXEL_RGB(0xAF, 0x52, 0xDE)
#define THEME_TEAL PIXEL_RGB(0x30, 0xB0, 0xC7)
#define THEME_INDIGO PIXEL_RGB(0x5E, 0x5C, 0xE6)

#define THEME_CONSOLE_BG PIXEL_RGB(0x14, 0x16, 0x1C)
#define THEME_CONSOLE_TEXT PIXEL_RGB(0xD6, 0xDA, 0xE4)
#define THEME_TERMINAL_BG PIXEL_RGB(0x1B, 0x1B, 0x20)
#define THEME_CONSOLE_BG PIXEL_RGB(0x14, 0x16, 0x1C)
#define THEME_CONSOLE_TEXT PIXEL_RGB(0xD6, 0xDA, 0xE4)
#define THEME_TERMINAL_BG_ALT PIXEL_RGB(0x24, 0x24, 0x2B)
#define THEME_TERMINAL_TEXT PIXEL_RGB(0xE8, 0xE8, 0xED)
#define THEME_TERMINAL_DIM PIXEL_RGB(0x8A, 0x8A, 0x95)
#define THEME_TERMINAL_PROMPT PIXEL_RGB(0x5A, 0xC8, 0xFA)
#define THEME_TERMINAL_CURSOR PIXEL_RGB(0x7A, 0xE7, 0xC8)

#define THEME_WALLPAPER_TOP PIXEL_RGB(0xE8, 0xEC, 0xF4)
#define THEME_WALLPAPER_MID PIXEL_RGB(0xD3, 0xDC, 0xEC)
#define THEME_WALLPAPER_BOTTOM PIXEL_RGB(0xBE, 0xC9, 0xDF)

#define THEME_FROST_MENUBAR_ALPHA 0xC4
#define THEME_FROST_DOCK_ALPHA 0xA8
#define THEME_FROST_WINDOW_ALPHA 0x00
#define THEME_FROST_TINT PIXEL_RGB(0xFF, 0xFF, 0xFF)
#define THEME_FROST_SATURATION 0.55

#define THEME_SHADOW_COLOR PIXEL_RGB(0x1B, 0x22, 0x33)

/* The chrome is translucent, and this is why.
 *
 * These surfaces are drawn over whatever the desktop background is, which is a
 * wallpaper the user chose rather than a colour this file decides.  Opaque and
 * near white, they were two glaring bars laid over a dark picture: not a bug in
 * the drawing but a panel that had never been over anything but a light
 * background.  Alpha lets the picture through, so the bar reads as a surface
 * floating over the desktop instead of pasted on top of it, and it looks the
 * same over the built in background as before.
 *
 * The text on them is light, because they are now dark-ish glass rather than
 * white paper. */
#define THEME_MENUBAR_FILL PIXEL_RGB(0x1A, 0x1D, 0x26)
#define THEME_MENUBAR_EDGE PIXEL_RGB(0xC2, 0xC7, 0xD2)
#define THEME_MENUBAR_SELECT PIXEL_RGB(0x3A, 0x44, 0x58)
#define THEME_MENU_FILL PIXEL_RGB(0xF2, 0xF3, 0xF7)
#define THEME_MENU_BORDER PIXEL_RGB(0xB6, 0xBC, 0xC9)
#define THEME_DOCK_FILL PIXEL_RGB(0x22, 0x26, 0x30)
#define THEME_DOCK_BORDER PIXEL_RGB(0x55, 0x5E, 0x70)

#define THEME_MENUBAR_HEIGHT 28
#define THEME_MENU_FONT_HEIGHT 8
#define THEME_MENU_ITEM_PAD 12
#define THEME_MENU_ITEM_GAP 2
#define THEME_MENU_ITEM_HEIGHT 22

#define THEME_DOCK_ICON 46
#define THEME_DOCK_ICON_GAP 10
#define THEME_DOCK_SEPARATOR_GAP 18
#define THEME_DOCK_PAD_X 12
#define THEME_DOCK_PAD_Y 8
#define THEME_DOCK_RADIUS 18
#define THEME_DOCK_MARGIN 10
#define THEME_DOCK_MAG 1.55
#define THEME_DOCK_HOVER_RANGE 130

#define THEME_WINDOW_RADIUS 11
#define THEME_TITLEBAR_HEIGHT 30
#define THEME_WINDOW_MIN_WIDTH 360
#define THEME_WINDOW_MIN_HEIGHT 220
#define THEME_WINDOW_SHADOW_SPREAD 22
#define THEME_WINDOW_SHADOW_LAYERS 7
#define THEME_WINDOW_BORDER 1

#define THEME_TRAFFIC_CLOSE PIXEL_RGB(0xFF, 0x5F, 0x57)
#define THEME_TRAFFIC_MIN PIXEL_RGB(0xFE, 0xBC, 0x2E)
#define THEME_TRAFFIC_MAX PIXEL_RGB(0x28, 0xC8, 0x40)
#define THEME_TRAFFIC_RING PIXEL_RGB(0x33, 0x33, 0x38)

#define THEME_CURSOR_SIZE 16
#define THEME_CURSOR_FILL PIXEL_RGB(0x00, 0x00, 0x00)
#define THEME_CURSOR_EDGE PIXEL_RGB(0xFF, 0xFF, 0xFF)

#endif
