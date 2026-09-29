#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "apps.h"
#include "font.h"
#include "gfx.h"
#include "heap.h"
#include "png.h"
#include "input.h"
#include "kby.h"
#include "launcher.h"
#include "lua_host.h"
#include "kernel.h"
#include "shell.h"
#include "theme.h"
#include "vfs.h"
#include "wm.h"

#define MENU_COUNT 6
#define MENU_ITEM_MAX 10
#define FRAME_INTERVAL_Q16 1092U
#define BOOT_ANIMATION_FRAMES 150

enum wm_menu_id {
    MENU_APPLE = 0,
    MENU_FILE,
    MENU_EDIT,
    MENU_VIEW,
    MENU_WINDOW,
    MENU_HELP,
    MENU_TOTAL
};

struct menu_item_def {
    const char *label;
    uint16_t action;
    uint8_t separator;
    uint8_t enabled;
};

struct menu_def {
    const char *title;
    const struct menu_item_def *items;
    uint32_t item_count;
};

#define WM_TITLE_MAX 48

struct wm_window {
    int used;
    int app;
    int script;
    int lua;
    /* Lua hosts have no enum app_id, so the title comes from their launcher
     * record.  Native and bytecode windows leave this empty and fall back to
     * the app or program name. */
    char title[WM_TITLE_MAX];
    int x;
    int y;
    int width;
    int height;
    int restore_x;
    int restore_y;
    int restore_width;
    int restore_height;
    int drag_offset_x;
    int drag_offset_y;
    int content_dirty;
    int z;
};

#define DOCK_SLOT_APP 0
#define DOCK_SLOT_SEPARATOR 1
#define DOCK_SLOT_LAUNCHER 2

struct dock_slot {
    int x;
    int width;
    int app;
    int separator;
    int kind;
    int index;
    int launcher;
};

struct wm_state {
    struct gfx_surface *back;
    struct gfx_surface *wallpaper;
    struct wm_window windows[WM_WINDOW_LIMIT];
    int window_count;
    int next_z;
    int focused;
    int active_menu;
    int active_menu_item;
    int pressed_menu;
    int mouse_x;
    int mouse_y;
    uint8_t mouse_buttons;
    int press_x;
    int press_y;
    int drag_window;
    int drag_target;
    uint8_t dragging;
    int dock_visible;
    int dock_hover;
    uint32_t dock_scale[APP_COUNT + 1];
    int dock_origin_x;
    int dock_width;
    int dock_y;
    struct dock_slot slots[APP_COUNT + LAUNCHER_MAX + 4];
    int slot_count;
    uint64_t last_frame_q16;
    uint32_t frame_count;
    uint32_t present_count;
    uint32_t fps;
    uint32_t fps_window_frames;
    uint32_t fps_window_start;
    uint64_t heartbeat[APP_COUNT];
    uint32_t chrome_dirty;
    uint32_t composite_count;
    /* how many window repaints were narrowed to a rectangle, for the shell
     * counters; the rest fell back to the whole window */
    uint32_t damage_rect_count;
    uint32_t ready;
};

static const struct menu_item_def menu_apple_items[] = {
    {"About Klye", WM_ACTION_ABOUT, 0, 1},
    {"", 0, 1, 0},
    {"System Settings...", WM_ACTION_SETTINGS, 0, 1},
    {"", 0, 1, 0},
    {"Restart...", WM_ACTION_REBOOT, 0, 1},
    {"Shut Down...", WM_ACTION_SHUTDOWN, 0, 1}
};

static const struct menu_item_def menu_file_items[] = {
    {"New Terminal", WM_ACTION_NEW_TERMINAL, 0, 1},
    {"New Text Editor", WM_ACTION_NEW_EDITOR, 0, 1},
    {"Open Files", WM_ACTION_OPEN_FILES, 0, 1},
    {"", 0, 1, 0},
    {"Close Window", WM_ACTION_CLOSE_WINDOW, 0, 1},
    {"Clear Terminal", WM_ACTION_CLEAR_TERMINAL, 0, 1}
};

static const struct menu_item_def menu_edit_items[] = {
    {"Undo", 0, 0, 0},
    {"Redo", 0, 0, 0},
    {"", 0, 1, 0},
    {"Cut", 0, 0, 0},
    {"Copy", 0, 0, 0},
    {"Paste", 0, 0, 0},
    {"Select All", 0, 0, 0}
};

static const struct menu_item_def menu_view_items[] = {
    {"Show Desktop", 0, 0, 0},
    {"", 0, 1, 0},
    {"Hide Dock", WM_ACTION_TOGGLE_DOCK, 0, 1}
};

static const struct menu_item_def menu_window_items[] = {
    {"Minimize", WM_ACTION_MINIMIZE_WINDOW, 0, 1},
    {"Zoom", WM_ACTION_ZOOM_WINDOW, 0, 1},
    {"", 0, 1, 0},
    {"Focus Next Window", WM_ACTION_FOCUS_NEXT, 0, 1}
};

static const struct menu_item_def menu_help_items[] = {
    {"Klye Help", WM_ACTION_SHOW_HELP, 0, 1},
    {"", 0, 1, 0},
    {"Command Reference", WM_ACTION_SHOW_HELP, 0, 1}
};

static const struct menu_def menus[MENU_TOTAL] = {
    {"klye", menu_apple_items, 6},
    {"File", menu_file_items, 6},
    {"Edit", menu_edit_items, 7},
    {"View", menu_view_items, 3},
    {"Window", menu_window_items, 4},
    {"Help", menu_help_items, 3}
};

#define MENU_APPLE_WIDTH 14
#define CURSOR_DAMAGE_PAD 2
#define CURSOR_DAMAGE_TOP_PAD 2
#define CURSOR_DAMAGE_W 20
#define CURSOR_DAMAGE_H 20
/* A solid pointer.  The previous bitmap was an outline, which read as a
 * transparent cursor over a light desktop. */
static const uint16_t cursor_shape[16] = {
    0x0001, 0x0003, 0x0007, 0x000F, 0x001F, 0x003F, 0x007F, 0x00FF,
    0x01FF, 0x03FF, 0x07FF, 0x0FFF, 0x1FFF, 0x1E7F, 0x1C7F, 0x1C7F
};

static struct wm_state wm;
static const char *wallpaper_error_text = "";
static struct gfx_surface wallpaper_surface;
static uint32_t *wallpaper_pixels;
static uint32_t animation_tick;

static int text_length(const char *text)
{
    int length = 0;

    while (text[length] != '\0') {
        length++;
    }
    return length;
}

#define FILES_SIDEBAR_W 190
#define FILES_GRID_X 204
#define FILES_GRID_Y 48
#define FILES_CELL_W 108
#define FILES_CELL_H 84
#define FILES_COLUMNS 4
#define FILES_ENTRIES 64
#define FILES_PLACE_COUNT 5
#define FILES_FOOTER 22

struct files_place {
    const char *label;
    const char *path;
};

static const struct files_place files_places[FILES_PLACE_COUNT] = {
    { "Home", "/home/klye" },
    { "System", "/etc" },
    { "Programs", "/bin" },
    { "Logs", "/var/log" },
    { "Root", "/" }
};

static char files_path[VFS_PATH_MAX] = "/home/klye";
static int files_selected;
static int files_top;

static int files_name_order(const char *a, const char *b)
{
    int index = 0;

    while (a[index] != '\0' && b[index] != '\0') {
        int left = a[index];
        int right = b[index];

        if (left >= 'a' && left <= 'z') {
            left -= 32;
        }
        if (right >= 'a' && right <= 'z') {
            right -= 32;
        }
        if (left != right) {
            return left < right ? -1 : 1;
        }
        index++;
    }
    if (a[index] == b[index]) {
        return 0;
    }
    return a[index] == '\0' ? -1 : 1;
}

static bool files_path_is(const char *path)
{
    return files_name_order(path, files_path) == 0;
}

static int files_count(void)
{
    int node = vfs_resolve(files_path, "/");

    return node < 0 ? 0 : vfs_child_count(node);
}

static int files_sorted(int *indices, int maximum)
{
    int found = vfs_list_path(files_path, indices, maximum);

    if (found > maximum) {
        found = maximum;
    }
    for (int outer = 1; outer < found; ++outer) {
        int node = indices[outer];
        bool node_dir = vfs_kind(node)[0] == 'd';
        int inner = outer - 1;

        while (inner >= 0) {
            bool other_dir = vfs_kind(indices[inner])[0] == 'd';
            int order;

            if (node_dir != other_dir) {
                order = node_dir ? -1 : 1;
            } else {
                order = files_name_order(vfs_name(node), vfs_name(indices[inner]));
            }
            if (order <= 0) {
                break;
            }
            indices[inner + 1] = indices[inner];
            inner--;
        }
        indices[inner + 1] = node;
    }
    return found;
}

static int files_visible_rows(int height)
{
    int rows = (height - FILES_GRID_Y - FILES_FOOTER - 8) / FILES_CELL_H;

    return rows < 1 ? 1 : rows;
}

static void files_clamp(int height)
{
    int indices[FILES_ENTRIES];
    int found = files_sorted(indices, FILES_ENTRIES);
    int rows = files_visible_rows(height);
    int max_top = found - rows * FILES_COLUMNS;

    if (files_selected >= found) {
        files_selected = found - 1;
    }
    if (files_selected < 0) {
        files_selected = 0;
    }
    if (max_top < 0) {
        max_top = 0;
    }
    if (files_top > max_top) {
        files_top = max_top;
    }
    if (files_top < 0) {
        files_top = 0;
    }
    if (files_selected < files_top * FILES_COLUMNS) {
        files_top = files_selected / FILES_COLUMNS;
    }
    if (files_selected >= (files_top + rows) * FILES_COLUMNS) {
        files_top = files_selected / FILES_COLUMNS - rows + 1;
    }
    if (files_top < 0) {
        files_top = 0;
    }
}

static void files_set_path(const char *path)
{
    int length = text_length(path);

    if (length <= 0 || length >= VFS_PATH_MAX) {
        return;
    }
    for (int index = 0; index < length; ++index) {
        files_path[index] = path[index];
    }
    files_path[length] = '\0';
    files_selected = 0;
    files_top = 0;
}

static void files_open(int slot)
{
    int indices[FILES_ENTRIES];
    int found = files_sorted(indices, FILES_ENTRIES);
    int node;
    char body[VFS_BODY_MAX];
    int length;

    if (slot < 0 || slot >= found) {
        return;
    }
    node = indices[slot];
    if (vfs_kind(node)[0] == 'd') {
        files_set_path(vfs_path(node));
        return;
    }
    length = vfs_read(vfs_path(node), body, (uint32_t)sizeof(body) - 1U);
    if (length < 0) {
        length = 0;
    }
    body[length] = '\0';
    editor_load(vfs_path(node), body);
    wm_launch_app(APP_EDITOR);
}

static void files_go_up(void)
{
    int parent = vfs_parent(vfs_resolve(files_path, "/"));

    if (parent >= 0) {
        files_set_path(vfs_path(parent));
    }
}

static void files_scroll(int delta, int height)
{
    files_top += delta;
    files_clamp(height);
}

static int files_slot_at(int local_x, int local_y, int height)
{
    int column;
    int row;
    int indices[FILES_ENTRIES];
    int slot;

    if (local_x < FILES_GRID_X || local_y < FILES_GRID_Y) {
        return -1;
    }
    column = (local_x - FILES_GRID_X) / FILES_CELL_W;
    row = (local_y - FILES_GRID_Y) / FILES_CELL_H;
    if (column >= FILES_COLUMNS || row >= files_visible_rows(height)) {
        return -1;
    }
    slot = (files_top + row) * FILES_COLUMNS + column;
    if (slot >= files_sorted(indices, FILES_ENTRIES)) {
        return -1;
    }
    return slot;
}


static void mark_chrome_dirty(void)
{
    wm.chrome_dirty = 1U;
}

/* Mark a window as needing a repaint.
 *
 * A script that changed only part of itself says which part, so the damage is
 * that rectangle rather than the window.  The shadow is still included, since
 * a window's edge is part of the same chrome and the shadow is drawn outside
 * the content.  Anything that cannot say, falls back to the whole window,
 * which is what this always did.
 */
static void damage_window(int index)
{
    struct wm_window *window = &wm.windows[index];
    int spread = THEME_WINDOW_SHADOW_SPREAD;
    struct gfx_rect part;

    if (window->used == 0) {
        return;
    }
    wm.composite_count++;
    window->content_dirty = 0;
    if (window->lua >= 0 &&
        lua_host_damage_rect(lua_host_at(window->lua), &part)) {
        gfx_damage(window->x + part.x - spread, window->y + part.y - spread,
                   part.width + spread * 2, part.height + spread * 2);
        wm.damage_rect_count++;
        return;
    }
    gfx_damage(window->x - spread, window->y - spread,
               window->width + spread * 2, window->height + spread * 2);
}

static void damage_rect(int x, int y, int width, int height)
{
    gfx_damage(x, y, width, height);
}

static int menubar_height(void)
{
    return THEME_MENUBAR_HEIGHT;
}

static int desktop_top(void)
{
    return menubar_height();
}

static int dock_box_height(void)
{
    return THEME_DOCK_ICON * 2 + THEME_DOCK_PAD_Y * 2;
}

static void build_wallpaper(void)
{
    uint32_t width = gfx_width();
    uint32_t height = gfx_height();
    int glow_x = (int)(width / 2);
    int glow_y = (int)(height / 4);

    gfx_gradient_v(wm.wallpaper, 0, 0, (int)width, (int)height,
                   THEME_WALLPAPER_TOP, THEME_WALLPAPER_BOTTOM);
    for (int dy = -150; dy <= 150; dy += 3) {
        int span = 150 - (dy < 0 ? -dy : dy);

        if (span <= 0) {
            continue;
        }
        gfx_blend_rect(wm.wallpaper, glow_x - span, glow_y + dy, span * 2, 3,
                       PIXEL_RGB(0xFF, 0xFF, 0xFF), 30U);
    }
    for (int index = 0; index < 3; ++index) {
        int offset_x = (int)width / 5 + index * (int)width / 3;
        int offset_y = (int)height - 170 - index * 46;

        for (int dy = -90; dy <= 90; dy += 3) {
            int span = 90 - (dy < 0 ? -dy : dy);

            if (span <= 0) {
                continue;
            }
            gfx_blend_rect(wm.wallpaper, offset_x - span, offset_y + dy,
                           span * 2, 3, PIXEL_RGB(0x8C, 0xA6, 0xDE), 30U);
        }
    }
    gfx_vignette(wm.wallpaper, 0, 0, (int)width, (int)height, 58U);
}

static int menu_title_x(int menu)
{
    int x = THEME_MENU_ITEM_PAD;

    if (menu == MENU_APPLE) {
        return x;
    }
    x += MENU_APPLE_WIDTH + THEME_MENU_ITEM_PAD * 2 + THEME_MENU_ITEM_GAP;
    for (int index = 1; index < menu; ++index) {
        int width = font_text_width(menus[index].title, 1);

        x += width + THEME_MENU_ITEM_PAD * 2 + THEME_MENU_ITEM_GAP;
    }
    return x;
}

static int menu_panel_width(const struct menu_def *menu)
{
    int width = 0;

    for (uint32_t index = 0; index < menu->item_count; ++index) {
        int item = font_text_width(menu->items[index].label, 1) + 44;

        if (item > width) {
            width = item;
        }
    }
    if (width < 170) {
        width = 170;
    }
    return width;
}

static int menu_panel_height(const struct menu_def *menu)
{
    int height = 10;

    for (uint32_t index = 0; index < menu->item_count; ++index) {
        height += menu->items[index].separator ? 9 : 22;
    }
    return height;
}

static void draw_desktop_icons(struct gfx_surface *surface)
{
    static const int icon_x = 40;
    int y = desktop_top() + 34;

    for (int index = APP_COUNT - 1; index >= 0; --index) {
        int label_width = font_text_width(app_name((enum app_id)index), 1);
        int label_x = icon_x + (THEME_DOCK_ICON - label_width) / 2;

        app_draw_icon(surface, (enum app_id)index, icon_x, y,
                      THEME_DOCK_ICON);
        gfx_rounded_rect(surface, label_x - 7, y + THEME_DOCK_ICON + 6,
                         label_width + 14, 15, 7,
                         PIXEL_RGB(0x1B, 0x22, 0x33));
        font_draw_centered(surface, icon_x + THEME_DOCK_ICON / 2,
                           y + THEME_DOCK_ICON + 18, app_name((enum app_id)index),
                           THEME_TEXT_ON_DARK, 1);
        y += THEME_DOCK_ICON + 46;
    }
}

static void draw_menubar(struct gfx_surface *surface)
{
    int height = menubar_height();
    char clock[24];
    int clock_width;
    int offset;

    gfx_fill(surface, 0, 0, (int)gfx_width(), height, THEME_MENUBAR_FILL);
    gfx_fill(surface, 0, 0, (int)gfx_width(), 1, THEME_MENUBAR_EDGE);
    gfx_fill(surface, 0, height - 1, (int)gfx_width(), 1,
             PIXEL_RGB(0x00, 0x00, 0x00));

    for (int menu = 0; menu < MENU_TOTAL; ++menu) {
        int x = menu_title_x(menu);
        int width = font_text_width(menus[menu].title, 1);
        int selected = wm.active_menu == menu;

        if (menu == MENU_APPLE) {
            gfx_circle(surface, x + 7, height / 2, 6, THEME_TEXT_PRIMARY);
            gfx_fill(surface, x + 4, height / 2 - 6, 2, 3, THEME_TEXT_PRIMARY);
            gfx_fill(surface, x + 9, height / 2 - 7, 2, 4, THEME_TEXT_PRIMARY);
            width = MENU_APPLE_WIDTH;
            if (selected) {
                gfx_rounded_rect(surface, x - 6, 4, width + 12, height - 8, 5,
                                 THEME_MENUBAR_SELECT);
            }
            if (wm.active_menu == MENU_APPLE) {
                gfx_circle(surface, x + 7, height / 2, 6, THEME_ACCENT_DEEP);
            }
            continue;
        }
        if (selected) {
            gfx_rounded_rect(surface, x - 6, 4, width + 12, height - 8, 5,
                             THEME_MENUBAR_SELECT);
        }
        font_draw(surface, x, height - 9, menus[menu].title,
                  selected ? THEME_ACCENT_DEEP : THEME_TEXT_PRIMARY, 1);
    }

    {
        uint64_t seconds = pit_ticks() / 1000U;
        uint32_t hours = (uint32_t)(seconds / 3600U) % 24U;
        uint32_t minutes = (uint32_t)(seconds / 60U) % 60U;
        static const char *const days[] = {
            "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
        };
        static const char *const months[] = {
            "Jan", "Feb", "Mar", "Apr", "May", "Jun",
            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
        };
        uint32_t day_index = (uint32_t)(seconds / 86400U) + 4U;
        int position = 0;

        clock[position++] = days[day_index % 7U][0];
        clock[position++] = days[day_index % 7U][1];
        clock[position++] = days[day_index % 7U][2];
        clock[position++] = ' ';
        clock[position++] = ' ';
        clock[position++] = months[day_index % 12U][0];
        clock[position++] = months[day_index % 12U][1];
        clock[position++] = months[day_index % 12U][2];
        clock[position++] = ' ';
        clock[position++] = (char)('0' + (days[day_index % 7U][0] == 'S' &&
                                         days[day_index % 7U][1] == 'a'
                                             ? 26
                                             : 1));
        clock[position++] = ' ';
        clock[position++] = (char)('0' + hours / 10U);
        clock[position++] = (char)('0' + hours % 10U);
        clock[position++] = ':';
        clock[position++] = (char)('0' + minutes / 10U);
        clock[position++] = (char)('0' + minutes % 10U);
        clock[position] = '\0';
        clock_width = font_text_width(clock, 1);
        offset = (int)gfx_width() - THEME_MENU_ITEM_PAD - clock_width;
        font_draw(surface, offset, height - 9, clock, THEME_TEXT_PRIMARY, 1);
        gfx_rounded_border(surface, offset - 14, height / 2 - 6, 8, 12, 2, 1,
                           THEME_TEXT_SECONDARY);
        gfx_fill(surface, offset - 12, height / 2 - 4, 4, 8, THEME_TEXT_SECONDARY);
        font_draw_right(surface, offset - 22, height - 9, "100%",
                        THEME_TEXT_PRIMARY, 1);
    }
}

static void draw_menu_panel(struct gfx_surface *surface)
{
    const struct menu_def *menu;
    int x;
    int y;
    int width;
    int height;
    int offset;

    if (wm.active_menu < 0 || wm.active_menu >= MENU_TOTAL) {
        return;
    }
    menu = &menus[wm.active_menu];
    x = menu_title_x(wm.active_menu) - 8;
    if (wm.active_menu == MENU_APPLE) {
        x = 4;
    }
    y = menubar_height() + 4;
    width = menu_panel_width(menu);
    height = menu_panel_height(menu);

    gfx_rounded_rect(surface, x, y, width, height, 12, THEME_MENU_FILL);
    gfx_rounded_border(surface, x, y, width, height, 12, 1,
                       THEME_MENU_BORDER);
    offset = 5;
    for (uint32_t index = 0; index < menu->item_count; ++index) {
        const struct menu_item_def *item = &menu->items[index];
        bool highlighted = wm.active_menu_item == (int)index &&
                           item->separator == 0;

        if (item->separator != 0) {
            gfx_fill(surface, x + 10, y + offset + 4, width - 20, 1,
                     THEME_SEPARATOR);
            offset += 9;
            continue;
        }
        if (highlighted) {
            gfx_rounded_rect(surface, x + 5, y + offset, width - 10, 21, 6,
                             THEME_ACCENT);
        }
        font_draw(surface, x + 14, y + offset + 15, item->label,
                  item->enabled ? (highlighted ? THEME_TEXT_ON_DARK
                                               : THEME_TEXT_PRIMARY)
                                : THEME_TEXT_TERTIARY,
                  1);
        offset += 22;
    }
}

static void layout_dock(void)
{
    int total = THEME_DOCK_PAD_X * 2;
    int count = 0;

    wm.slot_count = 0;
    for (int index = 0; index < APP_COUNT; ++index) {
        if (!app_is_open((enum app_id)index)) {
            continue;
        }
        count++;
    }
    if (count == 0 && launcher_count() == 0) {
        wm.dock_width = 0;
        wm.dock_origin_x = (int)gfx_width() / 2;
        return;
    }
    for (int index = 0; index < APP_COUNT; ++index) {
        if (!app_is_open((enum app_id)index)) {
            continue;
        }
        total += (THEME_DOCK_ICON * wm.dock_scale[index]) / 256 +
                 THEME_DOCK_ICON_GAP;
    }
    if (count != 0 && launcher_count() > 0) {
        total += THEME_DOCK_SEPARATOR_GAP;
    }
    for (int index = 0; index < launcher_count(); ++index) {
        total += (THEME_DOCK_ICON * wm.dock_scale[APP_COUNT - 1]) / 256 +
                 THEME_DOCK_ICON_GAP;
    }
    total -= THEME_DOCK_ICON_GAP;
    wm.dock_width = total;
    wm.dock_origin_x = (int)gfx_width() / 2 - total / 2;
    wm.dock_y = (int)gfx_height() - THEME_DOCK_MARGIN - dock_box_height();

    {
        int cursor = wm.dock_origin_x + THEME_DOCK_PAD_X;

        for (int index = 0; index < APP_COUNT; ++index) {
            if (!app_is_open((enum app_id)index)) {
                continue;
            }
            {
                int size = (THEME_DOCK_ICON * wm.dock_scale[index]) / 256;
                struct dock_slot *slot = &wm.slots[wm.slot_count++];

                slot->x = cursor;
                slot->width = size;
                slot->app = index;
                slot->separator = 0;
                slot->kind = DOCK_SLOT_APP;
                slot->index = wm.slot_count - 1;
                slot->launcher = -1;
                cursor += size + THEME_DOCK_ICON_GAP;
            }
        }
        if (count != 0 && launcher_count() > 0) {
            struct dock_slot *slot = &wm.slots[wm.slot_count++];

            slot->x = cursor;
            slot->width = THEME_DOCK_SEPARATOR_GAP;
            slot->app = -1;
            slot->separator = 1;
            slot->kind = DOCK_SLOT_SEPARATOR;
            slot->index = wm.slot_count - 1;
            slot->launcher = -1;
            cursor += THEME_DOCK_SEPARATOR_GAP;
        }
        for (int index = 0; index < launcher_count(); ++index) {
            int size = (THEME_DOCK_ICON * wm.dock_scale[APP_COUNT - 1]) / 256;
            struct dock_slot *slot = &wm.slots[wm.slot_count++];

            slot->x = cursor;
            slot->width = size;
            slot->app = -1;
            slot->separator = 0;
            slot->kind = DOCK_SLOT_LAUNCHER;
            slot->index = wm.slot_count - 1;
            slot->launcher = index;
            cursor += size + THEME_DOCK_ICON_GAP;
        }
    }
}

static void update_dock_animation(void)
{
    int changed = 0;
    int previous_hover = wm.dock_hover;

    wm.dock_hover = -1;
    for (int index = 0; index < wm.slot_count; ++index) {
        struct dock_slot *slot = &wm.slots[index];

        if (wm.mouse_x >= slot->x - 6 &&
            wm.mouse_x < slot->x + slot->width + 6 &&
            wm.mouse_y >= wm.dock_y &&
            wm.mouse_y < (int)gfx_height() - THEME_DOCK_MARGIN) {
            wm.dock_hover = slot->index;
        }
    }
    if (wm.dock_hover != previous_hover) {
        changed = 1;
    }
    for (int index = 0; index < APP_COUNT; ++index) {
        int target = 256;

        if (wm.dock_visible && wm.dock_hover == index) {
            target = (int)(256 * THEME_DOCK_MAG);
        }
        if ((int)wm.dock_scale[index] != target) {
            int delta = target - (int)wm.dock_scale[index];
            int step = delta / 5;

            if (step == 0) {
                step = delta > 0 ? 1 : -1;
            }
            wm.dock_scale[index] = (uint32_t)((int)wm.dock_scale[index] + step);
            if ((int)wm.dock_scale[index] > target) {
                wm.dock_scale[index] = (uint32_t)target;
            }
            if ((int)wm.dock_scale[index] < target) {
                wm.dock_scale[index] = (uint32_t)target;
            }
            changed = 1;
        }
    }
    if (changed) {
        damage_rect(0, wm.dock_y - 30, (int)gfx_width(),
                    (int)gfx_height() - wm.dock_y + 30);
    }
}

static void draw_launcher_icon(struct gfx_surface *surface,
                               const struct launcher *item, int x, int y,
                               int size)
{
    int radius = size / 5;
    char initial[2];

    gfx_rounded_rect(surface, x, y, size, size, radius,
                     PIXEL_RGB(0x2C, 0x36, 0x4C));
    gfx_rounded_border(surface, x, y, size, size, radius, 1,
                       PIXEL_RGB(0x4A, 0x57, 0x73));
    if (item == 0) {
        return;
    }
    if (item->icon >= 0) {
        app_draw_icon(surface, (enum app_id)item->icon, x, y, size);
        return;
    }
    initial[0] = item->title[0] != 0 ? item->title[0] : '?';
    initial[1] = 0;
    font_draw_centered(surface, x + size / 2, y + size / 2 + 6, initial,
                       THEME_TEXT_PRIMARY, 2);
}

static void draw_dock(struct gfx_surface *surface)
{
    int height;
    int y;

    if (!wm.dock_visible || wm.dock_width <= 0) {
        return;
    }
    height = dock_box_height();
    y = wm.dock_y;

    gfx_rounded_rect(surface, wm.dock_origin_x, y, wm.dock_width, height,
                     THEME_DOCK_RADIUS, THEME_DOCK_FILL);
    gfx_rounded_border(surface, wm.dock_origin_x, y, wm.dock_width, height,
                       THEME_DOCK_RADIUS, 1, THEME_DOCK_BORDER);

    for (int index = 0; index < wm.slot_count; ++index) {
        struct dock_slot *slot = &wm.slots[index];
        int size = slot->width;
        int top = y + (height - size) / 2;

        if (slot->kind == DOCK_SLOT_SEPARATOR) {
            gfx_fill(surface, slot->x + slot->width / 2 - 1, y + 8, 2,
                     height - 16, THEME_DOCK_BORDER);
            continue;
        }
        if (slot->kind == DOCK_SLOT_LAUNCHER) {
            const struct launcher *item = launcher_at(slot->launcher);
            const char *label = item != 0 ? item->title : "?";

            draw_launcher_icon(surface, item, slot->x, top, size);
            if (wm.dock_hover == slot->index) {
                int label_width = font_text_width(label, 1);
                int label_x = slot->x + size / 2 - label_width / 2;
                int label_y = y - 26;

                gfx_rounded_rect(surface, label_x - 9, label_y - 9,
                                 label_width + 18, 18, 8,
                                 PIXEL_RGB(0x1B, 0x22, 0x33));
                font_draw_centered(surface, slot->x + size / 2, label_y + 4,
                                   label, THEME_TEXT_ON_DARK, 1);
            }
            continue;
        }
        app_draw_icon(surface, (enum app_id)slot->app, slot->x, top, size);
        if (wm.dock_hover == slot->index) {
            int label_width = font_text_width(app_title((enum app_id)slot->app),
                                              1);
            int label_x = slot->x + size / 2 - label_width / 2;
            int label_y = y - 26;

            gfx_rounded_rect(surface, label_x - 9, label_y - 9,
                             label_width + 18, 18, 8,
                             PIXEL_RGB(0x1B, 0x22, 0x33));
            font_draw_centered(surface, slot->x + size / 2, label_y + 4,
                               app_title((enum app_id)slot->app),
                               THEME_TEXT_ON_DARK, 1);
        }
        if (app_is_open((enum app_id)slot->app)) {
            gfx_fill(surface, slot->x + size / 2 - 2, y + height - 5, 4, 3,
                     THEME_TEXT_SECONDARY);
        }
    }
}

static void window_content_size(enum app_id app, int *width, int *height)
{
    /* Sized for the text scale, not for a 1x font.
     *
     * A window is a fixed number of pixels, so doubling the text halves the
     * cells that fit across it: the terminal was 820 wide, which is 100 cells
     * at 8 pixels and 48 at 16.  Anything that wants a prompt, a command and
     * some output on one line was wrapping, and a wrapped neofetch drew its
     * logo down the screen one fragment per line. */
    switch (app) {
    case APP_TERMINAL:
        *width = 860;
        *height = 500;
        break;
    case APP_EDITOR:
        *width = 780;
        *height = 500;
        break;
    case APP_FILES:
        *width = 680;
        *height = 440;
        break;
    case APP_SETTINGS:
        *width = 620;
        *height = 420;
        break;
    case APP_BROWSER:
        *width = 720;
        *height = 460;
        break;
    case APP_ABOUT:
    default:
        *width = 460;
        *height = 320;
        break;
    }
}

static void draw_window_content(struct gfx_surface *surface,
                                struct wm_window *window)
{
    int x = window->x;
    int y = window->y + THEME_TITLEBAR_HEIGHT;
    int width = window->width;
    int height = window->height - THEME_TITLEBAR_HEIGHT;

    if (window->script >= 0) {
        kby_draw(kby_app_at(window->script), surface, x, y, width, height);
        return;
    }
    if (window->lua >= 0) {
        lua_host_draw(lua_host_at(window->lua), surface, x, y, width, height);
        return;
    }

    switch ((enum app_id)window->app) {
    case APP_TERMINAL:
        terminal_draw(surface, x, y, width, height);
        break;
    case APP_EDITOR:
        editor_draw(surface, x, y, width, height);
        break;
    case APP_FILES: {
        int indices[FILES_ENTRIES];
        int found;
        int rows;
        int footer_y = y + height - FILES_FOOTER;

        gfx_fill(surface, x, y, width, height, THEME_SURFACE);
        gfx_fill(surface, x, y, FILES_SIDEBAR_W, height, THEME_SURFACE_SUNKEN);
        gfx_vertical_line(surface, x + FILES_SIDEBAR_W, y, height,
                          THEME_SEPARATOR);
        font_draw(surface, x + 12, y + 20, "PLACES", THEME_TEXT_TERTIARY, 1);
        for (int place = 0; place < FILES_PLACE_COUNT; ++place) {
            int row = y + 40 + place * 28;
            bool active = files_path_is(files_places[place].path);

            gfx_rounded_rect(surface, x + 6, row, FILES_SIDEBAR_W - 12, 24, 6,
                             active ? THEME_ACCENT_SOFT : THEME_SURFACE_HOVER);
            font_draw(surface, x + 18, row + 16, files_places[place].label,
                      active ? THEME_ACCENT_DEEP : THEME_TEXT_PRIMARY, 1);
        }
        if (!files_path_is("/")) {
            gfx_rounded_rect(surface, x + 6,
                             y + 40 + FILES_PLACE_COUNT * 28,
                             FILES_SIDEBAR_W - 12, 24, 6, THEME_SURFACE_HOVER);
            font_draw(surface, x + 18, y + 40 + FILES_PLACE_COUNT * 28 + 16,
                      "Up one level", THEME_TEXT_PRIMARY, 1);
        }
        font_draw(surface, x + FILES_GRID_X, y + 20, files_path,
                  THEME_TEXT_PRIMARY, 1);
        {
            char count_text[16];
            int written = 0;
            int value = files_count();

            if (value <= 0) {
                count_text[written++] = '0';
            } else {
                char digits[8];
                int digit_count = 0;

                while (value > 0 && digit_count < (int)sizeof(digits) - 1) {
                    digits[digit_count++] = (char)('0' + (value % 10));
                    value /= 10;
                }
                for (int index = digit_count - 1; index >= 0; --index) {
                    count_text[written++] = digits[index];
                }
            }
            count_text[written++] = ' ';
            count_text[written++] = 'i';
            count_text[written++] = 't';
            count_text[written++] = 'e';
            count_text[written++] = 'm';
            count_text[written] = '\0';
            font_draw(surface, x + width - 16 - (int)text_length(count_text) * 6,
                      y + 20, count_text, THEME_TEXT_TERTIARY, 1);
        }
        gfx_horizontal_line(surface, x + FILES_GRID_X, y + 30, width - FILES_GRID_X - 16,
                            THEME_SEPARATOR);
        files_clamp(height);
        found = files_sorted(indices, FILES_ENTRIES);
        rows = files_visible_rows(height);
        for (int slot = files_top * FILES_COLUMNS;
             slot < found && slot < (files_top + rows) * FILES_COLUMNS; ++slot) {
            int node = indices[slot];
            bool is_dir = vfs_kind(node)[0] == 'd';
            int position = slot - files_top * FILES_COLUMNS;
            int cell_x = x + FILES_GRID_X + (position % FILES_COLUMNS) * FILES_CELL_W;
            int cell_y = y + FILES_GRID_Y + (position / FILES_COLUMNS) * FILES_CELL_H;
            char label[24];
            int length = text_length(vfs_name(node));
            int copy = length;
            int written = 0;

            if (slot == files_selected) {
                gfx_rounded_rect(surface, cell_x - 4, cell_y - 4,
                                 FILES_CELL_W - 8, FILES_CELL_H - 12, 8,
                                 THEME_ACCENT_SOFT);
            }
            if (is_dir) {
                gfx_fill(surface, cell_x + 30, cell_y + 5, 9, 5,
                         THEME_ACCENT_SOFT);
                gfx_rounded_rect(surface, cell_x + 26, cell_y + 8, 24, 20, 3,
                                 THEME_ACCENT_SOFT);
            } else {
                gfx_rounded_rect(surface, cell_x + 26, cell_y + 8, 22, 28, 3,
                                 THEME_SURFACE);
                gfx_rounded_border(surface, cell_x + 26, cell_y + 8, 22, 28, 3, 1,
                                   THEME_SEPARATOR_STRONG);
                font_draw_centered(surface, cell_x + 37, cell_y + 20, "TXT",
                                   THEME_TEXT_TERTIARY, 1);
            }
            if (copy > 20) {
                copy = 20;
            }
            for (int index = 0; index < copy && written < (int)sizeof(label) - 1;
                 ++index) {
                label[written++] = vfs_name(node)[index];
            }
            label[written] = '\0';
            font_draw_centered(surface, cell_x + 37, cell_y + 46, label,
                               THEME_TEXT_PRIMARY, 1);
            if (!is_dir) {
                char size_text[16];
                int written_size = 0;
                uint32_t size = vfs_size(node);
                char digits[12];
                int digit_count = 0;

                if (size == 0U) {
                    size_text[written_size++] = '0';
                } else {
                    while (size > 0U && digit_count < (int)sizeof(digits) - 1) {
                        digits[digit_count++] = (char)('0' + (size % 10U));
                        size /= 10U;
                    }
                }
                for (int index = digit_count - 1; index >= 0; --index) {
                    size_text[written_size++] = digits[index];
                }
                size_text[written_size++] = 'B';
                size_text[written_size] = '\0';
                font_draw_centered(surface, cell_x + 37, cell_y + 60, size_text,
                                   THEME_TEXT_TERTIARY, 1);
            }
        }
        gfx_fill(surface, x, footer_y, width, FILES_FOOTER, THEME_SURFACE_SUNKEN);
        gfx_horizontal_line(surface, x, footer_y, width, THEME_SEPARATOR);
        {
            char status[64];
            int offset = 0;

            for (int index = 0; index < (int)sizeof(status) - 1 && files_path[index] != '\0'; ++index) {
                status[offset++] = files_path[index];
            }
            if (offset < (int)sizeof(status) - 1) {
                status[offset++] = ' ';
            }
            if (found == 0) {
                for (int index = 0; index < 7 && offset < (int)sizeof(status); ++index) {
                    status[offset++] = "(empty)"[index];
                }
            } else {
                char selected_text[16];
                int written = 0;
                int value = files_selected + 1;
                char digits[8];
                int digit_count = 0;

                while (value > 0 && digit_count < (int)sizeof(digits) - 1) {
                    digits[digit_count++] = (char)('0' + (value % 10));
                    value /= 10;
                }
                for (int index = digit_count - 1; index >= 0; --index) {
                    selected_text[written++] = digits[index];
                }
                selected_text[written++] = '/';
                value = found;
                digit_count = 0;
                while (value > 0 && digit_count < (int)sizeof(digits) - 1) {
                    digits[digit_count++] = (char)('0' + (value % 10));
                    value /= 10;
                }
                for (int index = digit_count - 1; index >= 0; --index) {
                    selected_text[written++] = digits[index];
                }
                selected_text[written] = '\0';
                for (int index = 0; index < written && offset < (int)sizeof(status) - 1; ++index) {
                    status[offset++] = selected_text[index];
                }
            }
            status[offset] = '\0';
            font_draw(surface, x + 10, footer_y + 15, status,
                      THEME_TEXT_SECONDARY, 1);
        }
        break;
    }
    case APP_SETTINGS: {
        gfx_fill(surface, x, y, width, height, THEME_SURFACE);
        gfx_fill(surface, x, y, 168, height, THEME_SURFACE_SUNKEN);
        gfx_vertical_line(surface, x + 168, y, height, THEME_SEPARATOR);
        font_draw(surface, x + 12, y + 20, "GENERAL", THEME_TEXT_TERTIARY, 1);
        {
            static const char *const rows[] = {
                "Appearance", "Compositor", "Display", "Sound",
                "Keyboard", "About"
            };
            int row = y + 40;

            for (int index = 0; index < 6; ++index) {
                bool selected = index == 1;

                if (selected) {
                    gfx_rounded_rect(surface, x + 6, row, 156, 26, 6,
                                     THEME_ACCENT);
                }
                font_draw(surface, x + 18, row + 17, rows[index],
                          selected ? THEME_TEXT_ON_DARK : THEME_TEXT_PRIMARY, 1);
                row += 30;
            }
        }
        font_draw(surface, x + 188, y + 26, "Compositor", THEME_TEXT_PRIMARY, 1);
        font_draw(surface, x + 188, y + 40, "Display Engine", THEME_TEXT_TERTIARY,
                  1);
        {
            static const char *const labels[] = {
                "Frame pacing", "Damage tracking", "Presentation",
                "Rounded corners", "Frosted surfaces", "Dock magnification"
            };
            static const char *const values[] = {
                "60 fps (1000 hz timer)", "enabled", "double buffered",
                "anti-aliased", "enabled", "1.55x"
            };
            int row = y + 62;

            for (int index = 0; index < 6; ++index) {
                gfx_horizontal_line(surface, x + 188, row - 6, width - 210,
                                    THEME_SEPARATOR);
                font_draw(surface, x + 188, row + 10, labels[index],
                          THEME_TEXT_PRIMARY, 1);
                font_draw_right(surface, x + width - 24, row + 10, values[index],
                                THEME_TEXT_SECONDARY, 1);
                row += 34;
            }
        }
        break;
    }
    case APP_BROWSER: {
        gfx_fill(surface, x, y, width, height, THEME_SURFACE);
        gfx_fill(surface, x, y, width, height < 60 ? height : 60,
                 THEME_SURFACE_SUNKEN);
        gfx_fill(surface, x + 10, y + 14, width - 20, 30, THEME_SURFACE);
        gfx_rounded_border(surface, x + 10, y + 14, width - 20, 30, 8, 1,
                           THEME_SEPARATOR);
        font_draw(surface, x + 24, y + 33, "klye://welcome", THEME_ACCENT, 1);
        font_draw(surface, x + width - 40, y + 33, "x", THEME_TEXT_TERTIARY, 1);
        gfx_fill(surface, x, y + 60, width, height - 60, THEME_SURFACE);
        font_draw_centered(surface, x + width / 2, y + 120, "Klye Browser",
                           THEME_TEXT_PRIMARY, 3);
        font_draw_centered(surface, x + width / 2, y + 150,
                           "no pages loaded in this build", THEME_TEXT_TERTIARY,
                           1);
        gfx_rounded_rect(surface, x + width / 2 - 90, y + 180, 180, 34, 8,
                         THEME_ACCENT);
        font_draw_centered(surface, x + width / 2, y + 201, "Get Started",
                           THEME_TEXT_ON_DARK, 1);
        break;
    }
    case APP_ABOUT:
    default: {
        gfx_fill(surface, x, y, width, height, THEME_SURFACE);
        app_draw_icon(surface, APP_ABOUT, x + width / 2 - 40, y + 28, 80);
        font_draw_centered(surface, x + width / 2, y + 142, "Klye OS",
                           THEME_TEXT_PRIMARY, 2);
        font_draw_centered(surface, x + width / 2, y + 162, "Version 0.2",
                           THEME_TEXT_SECONDARY, 1);
        font_draw_centered(surface, x + width / 2, y + 186,
                           "freestanding x86_64 kernel", THEME_TEXT_TERTIARY, 1);
        gfx_horizontal_line(surface, x + 40, y + 204, width - 80,
                            THEME_SEPARATOR);
        font_draw_centered(surface, x + width / 2, y + 222, "compositor 60 fps",
                           THEME_TEXT_TERTIARY, 1);
        font_draw_centered(surface, x + width / 2, y + 234,
                           "damage tracked", THEME_TEXT_TERTIARY, 1);
        break;
    }
    }
}

static void draw_window(struct gfx_surface *surface, struct wm_window *window)
{
    int radius = THEME_WINDOW_RADIUS;
    int traffic_y = window->y + THEME_TITLEBAR_HEIGHT / 2;
    int close_x = window->x + 16;
    int minimize_x = close_x + 20;
    int zoom_x = minimize_x + 20;
    bool active = wm.focused == (int)(window - wm.windows);

    if (window->used == 0) {
        return;
    }
    gfx_rounded_rect(surface, window->x, window->y, window->width,
                     window->height, radius, THEME_SURFACE);
    gfx_fill(surface, window->x + radius, window->y + THEME_TITLEBAR_HEIGHT - 1,
             window->width - radius * 2, 1, THEME_SEPARATOR);
    gfx_rounded_border(surface, window->x, window->y, window->width,
                       window->height, radius, THEME_WINDOW_BORDER,
                       active ? PIXEL_RGB(0xC4, 0xC4, 0xCE)
                              : PIXEL_RGB(0xDD, 0xDD, 0xE4));

    {
        static const uint32_t lights[3] = {
            THEME_TRAFFIC_CLOSE, THEME_TRAFFIC_MIN, THEME_TRAFFIC_MAX
        };
        int centers[3];

        centers[0] = close_x;
        centers[1] = minimize_x;
        centers[2] = zoom_x;
        for (int index = 0; index < 3; ++index) {
            bool hovered = wm.mouse_y >= traffic_y - 9 &&
                           wm.mouse_y < traffic_y + 9 &&
                           wm.mouse_x >= centers[index] - 9 &&
                           wm.mouse_x < centers[index] + 9;

            if (!active) {
                gfx_circle(surface, centers[index], traffic_y, 6,
                           PIXEL_RGB(0xE4, 0xE4, 0xEA));
                continue;
            }
            gfx_circle(surface, centers[index], traffic_y, 6,
                       hovered ? gfx_shade(lights[index], 18) : lights[index]);
            gfx_ring(surface, centers[index], traffic_y, 6, 1,
                     THEME_TRAFFIC_RING);
        }
    }
    {
        const char *label = window->title[0] != 0
                                ? window->title
                                : (window->script >= 0
                                       ? kby_app_title(kby_app_at(window->script))
                                       : app_title((enum app_id)window->app));

        font_draw(surface, window->x + 74, traffic_y + 3, label,
                  active ? THEME_TEXT_PRIMARY : THEME_TEXT_SECONDARY, 1);
    }
    if (active) {
        gfx_fill(surface, window->x + 60, traffic_y - 1, 8, 2,
                 THEME_ACCENT);
    }
    draw_window_content(surface, window);
}

static void draw_cursor_overlay_region(struct gfx_surface *surface,
                                       const struct gfx_rect *r);

static void redraw_region(struct gfx_surface *surface, const struct gfx_rect *r)
{
    if (gfx_rect_empty(r)) {
        return;
    }
    gfx_blit(surface, r->x, r->y, r->width, r->height, wm.wallpaper, r->x,
             r->y);
    draw_desktop_icons(surface);

    {
        struct wm_window *ordered[WM_WINDOW_LIMIT];
        int count = 0;

        for (int index = 0; index < WM_WINDOW_LIMIT; ++index) {
            if (wm.windows[index].used != 0) {
                ordered[count++] = &wm.windows[index];
            }
        }
        for (int outer = 0; outer < count; ++outer) {
            for (int inner = outer + 1; inner < count; ++inner) {
                if (ordered[inner]->z < ordered[outer]->z) {
                    struct wm_window *swap = ordered[outer];

                    ordered[outer] = ordered[inner];
                    ordered[inner] = swap;
                }
            }
        }
        for (int index = 0; index < count; ++index) {
            struct wm_window *window = ordered[index];
            struct gfx_rect bounds;

            bounds.x = window->x - THEME_WINDOW_SHADOW_SPREAD;
            bounds.y = window->y - THEME_WINDOW_SHADOW_SPREAD;
            bounds.width = window->width + THEME_WINDOW_SHADOW_SPREAD * 2;
            bounds.height = window->height + THEME_WINDOW_SHADOW_SPREAD * 2;
            if (!gfx_rect_overlaps(r, &bounds)) {
                continue;
            }
            draw_window(surface, window);
        }
    }
    {
        struct gfx_rect menubar_rect;
        struct gfx_rect dock_rect;

        menubar_rect.x = 0;
        menubar_rect.y = 0;
        menubar_rect.width = (int)gfx_width();
        menubar_rect.height = menubar_height();
        if (gfx_rect_overlaps(r, &menubar_rect)) {
            draw_menubar(surface);
        }
        dock_rect.x = 0;
        dock_rect.y = wm.dock_y - 34;
        dock_rect.width = (int)gfx_width();
        dock_rect.height = (int)gfx_height() - dock_rect.y;
        if (wm.dock_visible && gfx_rect_overlaps(r, &dock_rect)) {
            draw_dock(surface);
        }
    }
    {
        struct gfx_rect panel;

        if (wm.active_menu >= 0) {
            const struct menu_def *menu = &menus[wm.active_menu];

            panel.x = wm.active_menu == MENU_APPLE
                          ? 4
                          : menu_title_x(wm.active_menu) - 8;
            panel.y = menubar_height() + 4;
            panel.width = menu_panel_width(menu);
            panel.height = menu_panel_height(menu) + 30;
            if (gfx_rect_overlaps(r, &panel)) {
                draw_menu_panel(surface);
            }
        }
    }
    draw_cursor_overlay_region(surface, r);
}

static void draw_cursor_overlay_region(struct gfx_surface *surface,
                                       const struct gfx_rect *r)
{
    for (int row = 0; row < 16; ++row) {
        for (int column = 0; column < 16; ++column) {
            int x = wm.mouse_x + column;
            int y = wm.mouse_y + row;
            struct gfx_rect pixel;

            if ((cursor_shape[row] & (1U << column)) == 0U) {
                continue;
            }
            pixel.x = x;
            pixel.y = y;
            pixel.width = 1;
            pixel.height = 1;
            if (gfx_rect_overlaps(r, &pixel) == false) {
                continue;
            }
            /* solid black pointer, with a one pixel white edge so it stays
             * visible over both the light desktop and the black boot screen */
            gfx_fill(surface, x, y, 1, 1, THEME_CURSOR_FILL);
            if ((row > 0 && (cursor_shape[row - 1] & (1U << column)) == 0U) ||
                (row < 15 && (cursor_shape[row + 1] & (1U << column)) == 0U) ||
                (column > 0 && (cursor_shape[row] & (1U << (column - 1))) == 0U) ||
                (column < 15 && (cursor_shape[row] & (1U << (column + 1))) == 0U)) {
                gfx_fill(surface, x, y, 1, 1, THEME_CURSOR_EDGE);
            }
        }
    }
}

static void draw_cursor_overlay(struct gfx_surface *surface)
{
    struct gfx_rect all;

    all.x = 0;
    all.y = 0;
    all.width = (int)gfx_width();
    all.height = (int)gfx_height();
    draw_cursor_overlay_region(surface, &all);
}

static void full_recompose(void)
{
    struct gfx_rect full;

    full.x = 0;
    full.y = 0;
    full.width = (int)gfx_width();
    full.height = (int)gfx_height();
    gfx_damage_all();
    redraw_region(wm.back, &full);
    draw_cursor_overlay(wm.back);
    wm.chrome_dirty = 0;
    wm.composite_count++;
}

static uint64_t read_tsc(void)
{
    uint32_t low;
    uint32_t high;

    __asm__ __volatile__("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | (uint64_t)low;
}

static void log_u64(uint64_t value)
{
    char digits[24];
    int length = 0;
    char text[24];

    if (value == 0U) {
        serial_write("0");
        return;
    }
    while (value > 0U && length < 20) {
        digits[length] = (char)('0' + (int)(value % 10U));
        value /= 10U;
        length++;
    }
    for (int index = 0; index < length; ++index) {
        text[index] = digits[length - 1 - index];
    }
    text[length] = '\0';
    serial_write(text);
}

void wm_benchmark(uint32_t iterations)
{
    uint64_t start;
    uint64_t elapsed;
    uint64_t per_frame;
    uint64_t tick_start;
    uint64_t tick_end;
    uint64_t tick_span;
    uint32_t present_before;
    uint32_t rows_after;

    if (iterations == 0U) {
        iterations = 30U;
    }
    present_before = gfx_present_count();
    tick_start = wm_uptime_ticks();
    start = read_tsc();
    for (uint32_t index = 0; index < iterations; ++index) {
        full_recompose();
    }
    elapsed = read_tsc() - start;
    tick_end = wm_uptime_ticks();
    per_frame = elapsed / (uint64_t)iterations;
    tick_span = tick_end - tick_start;
    rows_after = gfx_last_present_rows();

    serial_write("BENCH full-compose frames=");
    log_u64((uint64_t)iterations);
    serial_write(" total_cycles=");
    log_u64(elapsed);
    serial_write(" cycles_per_frame=");
    log_u64(per_frame);
    serial_write(" ms_total=");
    log_u64(tick_span);
    serial_write(" ms_per_frame=");
    log_u64(tick_span / (uint64_t)iterations);
    serial_write(" fps=");
    log_u64(tick_span == 0U ? 0U
                            : ((uint64_t)iterations * 1000U) / tick_span);
    serial_write(" present_rows=");
    log_u64((uint64_t)rows_after);
    serial_write(" presents=");
    log_u64((uint64_t)(gfx_present_count() - present_before));
    serial_write("\n");
    gfx_damage_all();
    wm.chrome_dirty = 1;
}

static void repaint_damage(void)
{
    struct gfx_rect region;

    if (wm.chrome_dirty != 0U) {
        full_recompose();
        return;
    }
    if (!gfx_damage_region(&region)) {
        return;
    }
    redraw_region(wm.back, &region);
}

static int window_at(int x, int y)
{
    int best = -1;
    int best_z = -1;

    for (int index = 0; index < WM_WINDOW_LIMIT; ++index) {
        struct wm_window *window = &wm.windows[index];
        struct gfx_rect bounds;

        if (window->used == 0) {
            continue;
        }
        bounds.x = window->x;
        bounds.y = window->y;
        bounds.width = window->width;
        bounds.height = window->height;
        if (!gfx_rect_contains(&bounds, x, y)) {
            continue;
        }
        if (window->z >= best_z) {
            best_z = window->z;
            best = index;
        }
    }
    return best;
}

static void focus_window(int index)
{
    if (index < 0) {
        return;
    }
    wm.focused = index;
    wm.next_z++;
    wm.windows[index].z = wm.next_z;
    mark_chrome_dirty();
}

static int find_window_for_app(int app)
{
    for (int index = 0; index < WM_WINDOW_LIMIT; ++index) {
        if (wm.windows[index].used != 0 && wm.windows[index].app == app &&
            wm.windows[index].script < 0) {
            return index;
        }
    }
    return -1;
}

static int find_window_for_script(int script)
{
    for (int index = 0; index < WM_WINDOW_LIMIT; ++index) {
        if (wm.windows[index].used != 0 && wm.windows[index].script == script) {
            return index;
        }
    }
    return -1;
}

static int find_window_for_lua(int host)
{
    for (int index = 0; index < WM_WINDOW_LIMIT; ++index) {
        if (wm.windows[index].used != 0 && wm.windows[index].lua == host) {
            return index;
        }
    }
    return -1;
}

static void close_window(int index)
{
    if (index < 0 || index >= WM_WINDOW_LIMIT) {
        return;
    }
    if (wm.windows[index].script >= 0) {
        kby_unload(kby_app_at(wm.windows[index].script));
    } else if (wm.windows[index].lua >= 0) {
        lua_host_unload(lua_host_at(wm.windows[index].lua));
    } else {
        app_close((enum app_id)wm.windows[index].app);
    }
    wm.windows[index].used = 0;
    wm.windows[index].script = -1;
    wm.windows[index].lua = -1;
    if (wm.focused == index) {
        wm.focused = -1;
        for (int scan = WM_WINDOW_LIMIT - 1; scan >= 0; --scan) {
            if (wm.windows[scan].used != 0) {
                wm.focused = scan;
                break;
            }
        }
    }
    mark_chrome_dirty();
}

void wm_launch_lua(int host_index, const char *title, int width, int height)
{
    int slot = -1;
    int existing = find_window_for_lua(host_index);

    if (existing >= 0) {
        focus_window(existing);
        mark_chrome_dirty();
        return;
    }
    for (int index = 0; index < WM_WINDOW_LIMIT; ++index) {
        if (wm.windows[index].used == 0) {
            slot = index;
            break;
        }
    }
    if (slot < 0) {
        return;
    }
    if (width > (int)gfx_width() - 80) {
        width = (int)gfx_width() - 80;
    }
    if (height > (int)gfx_height() - 160) {
        height = (int)gfx_height() - 160;
    }
    /* Wipe the slot the moment it is claimed.  Doing this after the position
     * is computed erased it, which parked every new window at 0,0 underneath
     * the menubar where its title bar could not be grabbed. */
    __builtin_memset(&wm.windows[slot], 0, sizeof(wm.windows[slot]));
    {
        int offset = wm.window_count * 26;

        if (offset > 150) {
            offset = 150;
        }
        wm.windows[slot].x = 150 + offset;
        /* Start well clear of the menubar: a window whose title bar sits
         * against it is awkward to grab, because the bar is the only part
         * you can drag. */
        wm.windows[slot].y = desktop_top() + 72 + offset;
    }
    wm.windows[slot].used = 1;
    wm.windows[slot].app = APP_ABOUT;
    wm.windows[slot].script = -1;
    wm.windows[slot].lua = host_index;
    if (title != 0) {
        int at = 0;

        while (title[at] != 0 && at < WM_TITLE_MAX - 1) {
            wm.windows[slot].title[at] = title[at];
            ++at;
        }
        wm.windows[slot].title[at] = 0;
    }
    wm.windows[slot].width = width;
    wm.windows[slot].height = height;
    wm.windows[slot].restore_x = wm.windows[slot].x;
    wm.windows[slot].restore_y = wm.windows[slot].y;
    wm.windows[slot].restore_width = width;
    wm.windows[slot].restore_height = height;
    wm.windows[slot].content_dirty = 1;
    wm.window_count++;
    wm.next_z++;
    wm.windows[slot].z = wm.next_z;
    wm.focused = slot;
    mark_chrome_dirty();
    (void)title;
}

void wm_launch_script(int script_index, const char *title, int width,
                      int height)
{
    int slot = -1;
    int existing = find_window_for_script(script_index);

    if (existing >= 0) {
        focus_window(existing);
        mark_chrome_dirty();
        return;
    }
    for (int index = 0; index < WM_WINDOW_LIMIT; ++index) {
        if (wm.windows[index].used == 0) {
            slot = index;
            break;
        }
    }
    if (slot < 0) {
        return;
    }
    if (width > (int)gfx_width() - 80) {
        width = (int)gfx_width() - 80;
    }
    if (height > (int)gfx_height() - 160) {
        height = (int)gfx_height() - 160;
    }
    /* wipe the slot the moment it is claimed, before the position is written:
     * doing it afterwards erased the position and parked the window at 0,0
     * under the menubar, where its title bar cannot be grabbed */
    __builtin_memset(&wm.windows[slot], 0, sizeof(wm.windows[slot]));
    {
        int offset = wm.window_count * 26;

        if (offset > 150) {
            offset = 150;
        }
        wm.windows[slot].x = 120 + offset;
        wm.windows[slot].y = desktop_top() + 34 + offset;
    }
    wm.windows[slot].used = 1;
    wm.windows[slot].app = 0;
    wm.windows[slot].script = script_index;
    wm.windows[slot].lua = -1;
    wm.windows[slot].width = width;
    wm.windows[slot].height = height;
    wm.windows[slot].restore_x = wm.windows[slot].x;
    wm.windows[slot].restore_y = wm.windows[slot].y;
    wm.windows[slot].restore_width = width;
    wm.windows[slot].restore_height = height;
    wm.windows[slot].content_dirty = 1;
    wm.window_count++;
    wm.next_z++;
    wm.windows[slot].z = wm.next_z;
    wm.focused = slot;
    mark_chrome_dirty();
    (void)title;
}

static int script_self_slot = -1;

bool kby_open_window(const char *title, int width, int height)
{
    struct kby_app *app = kby_find(title);

    if (app == 0) {
        return false;
    }
    script_self_slot = kby_app_slot(app);
    wm_launch_script(script_self_slot, title, width, height);
    return true;
}

void kby_close_self(void)
{
    if (script_self_slot < 0) {
        return;
    }
    {
        int slot = -1;

        for (int index = 0; index < WM_WINDOW_LIMIT; ++index) {
            if (wm.windows[index].used != 0 &&
                wm.windows[index].script == script_self_slot) {
                slot = index;
                break;
            }
        }
        if (slot >= 0) {
            close_window(slot);
        }
    }
    script_self_slot = -1;
}

void wm_launch_app(enum app_id app)
{
    int existing = find_window_for_app(app);
    int width;
    int height;
    int slot = -1;

    app_open(app);
    if (existing >= 0) {
        focus_window(existing);
        mark_chrome_dirty();
        return;
    }
    window_content_size(app, &width, &height);
    for (int index = 0; index < WM_WINDOW_LIMIT; ++index) {
        if (wm.windows[index].used == 0) {
            slot = index;
            break;
        }
    }
    if (slot < 0) {
        return;
    }
    if (width > (int)gfx_width() - 80) {
        width = (int)gfx_width() - 80;
    }
    if (height > (int)gfx_height() - 160) {
        height = (int)gfx_height() - 160;
    }
    /* wipe the slot when it is claimed, before anything is written into it */
    __builtin_memset(&wm.windows[slot], 0, sizeof(wm.windows[slot]));
    {
        int offset = wm.window_count * 26;

        if (offset > 150) {
            offset = 150;
        }
        wm.windows[slot].x = 120 + offset;
        wm.windows[slot].y = desktop_top() + 34 + offset;
    }
    wm.windows[slot].used = 1;
    wm.windows[slot].app = app;
    wm.windows[slot].script = -1;
    wm.windows[slot].lua = -1;
    wm.windows[slot].width = width;
    wm.windows[slot].height = height;
    wm.windows[slot].restore_x = wm.windows[slot].x;
    wm.windows[slot].restore_y = wm.windows[slot].y;
    wm.windows[slot].restore_width = width;
    wm.windows[slot].restore_height = height;
    wm.windows[slot].content_dirty = 1;
    wm.window_count++;
    wm.next_z++;
    wm.windows[slot].z = wm.next_z;
    wm.focused = slot;
    wm.heartbeat[app] = pit_ticks();
    mark_chrome_dirty();
}

void wm_close_focused(void)
{
    if (wm.focused >= 0) {
        close_window(wm.focused);
    }
}

void wm_minimize_focused(void)
{
    if (wm.focused < 0) {
        return;
    }
    app_close((enum app_id)wm.windows[wm.focused].app);
    wm.windows[wm.focused].used = 0;
    wm.focused = -1;
    for (int index = WM_WINDOW_LIMIT - 1; index >= 0; --index) {
        if (wm.windows[index].used != 0) {
            wm.focused = index;
            break;
        }
    }
    mark_chrome_dirty();
}

void wm_run_action(enum wm_action action)
{
    switch (action) {
    case WM_ACTION_ABOUT:
        wm_launch_app(APP_ABOUT);
        break;
    case WM_ACTION_SETTINGS:
        wm_launch_app(APP_SETTINGS);
        break;
    case WM_ACTION_NEW_TERMINAL:
        terminal_clear();
        wm_launch_app(APP_TERMINAL);
        break;
    case WM_ACTION_NEW_EDITOR:
        wm_launch_app(APP_EDITOR);
        break;
    case WM_ACTION_OPEN_FILES:
        wm_launch_app(APP_FILES);
        break;
    case WM_ACTION_CLOSE_WINDOW:
        wm_close_focused();
        break;
    case WM_ACTION_MINIMIZE_WINDOW:
        wm_minimize_focused();
        break;
    case WM_ACTION_ZOOM_WINDOW:
        if (wm.focused >= 0) {
            struct wm_window *window = &wm.windows[wm.focused];

            if (window->width < (int)gfx_width() - 100) {
                window->restore_x = window->x;
                window->restore_y = window->y;
                window->restore_width = window->width;
                window->restore_height = window->height;
                window->x = 40;
                window->y = desktop_top() + 16;
                window->width = (int)gfx_width() - 80;
                window->height = (int)gfx_height() - desktop_top() - 110;
            } else {
                window->x = window->restore_x;
                window->y = window->restore_y;
                window->width = window->restore_width;
                window->height = window->restore_height;
            }
            window->content_dirty = 1;
            mark_chrome_dirty();
        }
        break;
    case WM_ACTION_TOGGLE_DOCK:
        wm_toggle_dock();
        break;
    case WM_ACTION_SHOW_HELP:
        terminal_clear();
        terminal_puts("Klye Help\n");
        terminal_puts("=========\n\n");
        terminal_puts("Click the Klye menu for system actions.\n");
        terminal_puts("Click the dock to launch applications.\n");
        terminal_puts("Drag a title bar to move a window.\n");
        terminal_puts("Use the red, yellow and green lights.\n");
        terminal_puts("Type 'help' here for the command list.\n");
        wm_launch_app(APP_TERMINAL);
        break;
    case WM_ACTION_CLEAR_TERMINAL:
        terminal_clear();
        mark_chrome_dirty();
        break;
    case WM_ACTION_FOCUS_NEXT: {
        int best = -1;
        int best_z = -1;

        for (int index = 0; index < WM_WINDOW_LIMIT; ++index) {
            if (wm.windows[index].used != 0 && index != wm.focused &&
                wm.windows[index].z > best_z) {
                best_z = wm.windows[index].z;
                best = index;
            }
        }
        focus_window(best);
        break;
    }
    case WM_ACTION_REBOOT:
        terminal_puts("restarting...\n");
        shell_machine_reboot();
        break;
    case WM_ACTION_SHUTDOWN:
        terminal_puts("shutting down...\n");
        shell_machine_halt();
        break;
    case WM_ACTION_NONE:
    default:
        break;
    }
}

bool wm_menu_is_open(void)
{
    return wm.active_menu >= 0;
}

bool wm_dock_visible(void)
{
    return wm.dock_visible != 0;
}

void wm_toggle_dock(void)
{
    wm.dock_visible = !wm.dock_visible;
    mark_chrome_dirty();
}

int wm_focused_window(void)
{
    return wm.focused;
}

int wm_open_window_count(void)
{
    return wm.window_count;
}

uint64_t wm_app_heartbeat(enum app_id app)
{
    if (app >= APP_COUNT) {
        return 0U;
    }
    return pit_ticks() - wm.heartbeat[app];
}

uint32_t wm_fps(void)
{
    return wm.fps;
}

uint32_t wm_frame_count(void)
{
    return wm.frame_count;
}

uint32_t wm_present_count(void)
{
    return wm.present_count;
}

uint64_t wm_uptime_ticks(void)
{
    return pit_ticks();
}

void wm_set_ready(bool ready)
{
    wm.ready = ready ? 1U : 0U;
}

static int menu_at(int x, int y)
{
    if (y < 0 || y >= menubar_height()) {
        return -1;
    }
    for (int menu = 0; menu < MENU_TOTAL; ++menu) {
        int title_x = menu_title_x(menu);
        int width = menu == MENU_APPLE
                        ? MENU_APPLE_WIDTH
                        : font_text_width(menus[menu].title, 1);

        if (x >= title_x - 8 && x < title_x + width + 8) {
            return menu;
        }
    }
    return -1;
}

static int menu_item_at(int x, int y)
{
    const struct menu_def *menu;
    int panel_x;
    int panel_y;
    int width;
    int offset;

    if (wm.active_menu < 0) {
        return -1;
    }
    menu = &menus[wm.active_menu];
    panel_x = wm.active_menu == MENU_APPLE ? 4 : menu_title_x(wm.active_menu) - 8;
    panel_y = menubar_height() + 4;
    width = menu_panel_width(menu);
    if (x < panel_x || x >= panel_x + width) {
        return -1;
    }
    if (y < panel_y || y >= panel_y + menu_panel_height(menu)) {
        return -1;
    }
    offset = 5;
    for (uint32_t index = 0; index < menu->item_count; ++index) {
        int item_y = panel_y + offset;
        int item_height = menu->items[index].separator ? 9 : 22;

        if (y >= item_y && y < item_y + item_height) {
            return menu->items[index].separator ? -1 : (int)index;
        }
        offset += item_height;
    }
    return -1;
}

static int dock_slot_at(int x, int y)
{
    if (!wm.dock_visible || y < wm.dock_y ||
        y >= (int)gfx_height() - THEME_DOCK_MARGIN) {
        return -1;
    }
    for (int index = 0; index < wm.slot_count; ++index) {
        struct dock_slot *slot = &wm.slots[index];

        if (x >= slot->x - 4 && x < slot->x + slot->width + 4) {
            return index;
        }
    }
    return -1;
}

static int desktop_icon_at(int x, int y)
{
    int icon_y = desktop_top() + 34;

    for (int scan = APP_COUNT - 1; scan >= 0; --scan) {
        if (x >= 40 && x < 40 + THEME_DOCK_ICON && y >= icon_y &&
            y < icon_y + THEME_DOCK_ICON + 24) {
            return scan;
        }
        icon_y += THEME_DOCK_ICON + 46;
    }
    return -1;
}

static void handle_mouse_press(void)
{
    int x = wm.press_x;
    int y = wm.press_y;
    int menu;
    int item;
    int slot;
    int icon;
    int index;

    if ((wm.mouse_buttons & MOUSE_BUTTON_LEFT) == 0U) {
        return;
    }
    menu = menu_at(x, y);
    if (menu >= 0) {
        wm.active_menu = wm.active_menu == menu ? -1 : menu;
        wm.active_menu_item = -1;
        wm.pressed_menu = wm.active_menu;
        mark_chrome_dirty();
        return;
    }
    item = menu_item_at(x, y);
    if (item >= 0) {
        const struct menu_def *definition = &menus[wm.active_menu];

        if (definition->items[item].enabled != 0U) {
            enum wm_action action = (enum wm_action)definition->items[item].action;

            wm.active_menu = -1;
            wm.active_menu_item = -1;
            mark_chrome_dirty();
            wm_run_action(action);
        }
        return;
    }
    if (wm.active_menu >= 0) {
        wm.active_menu = -1;
        wm.active_menu_item = -1;
        mark_chrome_dirty();
    }
    index = window_at(x, y);
    if (index >= 0) {
        struct wm_window *window = &wm.windows[index];
        int traffic_y = window->y + THEME_TITLEBAR_HEIGHT / 2;

        if (y < window->y + THEME_TITLEBAR_HEIGHT) {
            if (x >= window->x + 6 && x < window->x + 26) {
                focus_window(index);
                close_window(index);
                return;
            }
            if (x >= window->x + 26 && x < window->x + 46) {
                focus_window(index);
                wm_minimize_focused();
                return;
            }
            if (x >= window->x + 46 && x < window->x + 66) {
                focus_window(index);
                wm_run_action(WM_ACTION_ZOOM_WINDOW);
                return;
            }
            if (y >= traffic_y - 10 && y < traffic_y + 10) {
                focus_window(index);
                return;
            }
            focus_window(index);
            wm.drag_window = index;
            wm.dragging = 1U;
            window->drag_offset_x = x - window->x;
            window->drag_offset_y = y - window->y;
            return;
        }
        focus_window(index);
        if (window->app == APP_FILES) {
            int content_y = window->y + THEME_TITLEBAR_HEIGHT;
            int content_h = window->height - THEME_TITLEBAR_HEIGHT;
            int local_x = x - window->x;
            int local_y = y - content_y;
            int place;

            if (local_x < FILES_SIDEBAR_W) {
                for (place = 0; place < FILES_PLACE_COUNT; ++place) {
                    int row = 40 + place * 28;

                    if (local_y >= row && local_y < row + 24) {
                        files_set_path(files_places[place].path);
                        window->content_dirty = 1;
                        return;
                    }
                }
                if (!files_path_is("/") && local_y >= 40 + FILES_PLACE_COUNT * 28 &&
                    local_y < 40 + FILES_PLACE_COUNT * 28 + 24) {
                    files_go_up();
                    window->content_dirty = 1;
                }
                return;
            }
            {
                int slot_hit = files_slot_at(local_x, local_y, content_h);

                if (slot_hit >= 0) {
                    files_selected = slot_hit;
                    files_open(slot_hit);
                    window->content_dirty = 1;
                }
            }
        }
        return;
    }
    slot = dock_slot_at(x, y);
    if (slot >= 0) {
        const struct dock_slot *entry = &wm.slots[slot];

        if (entry->kind == DOCK_SLOT_LAUNCHER) {
            const struct launcher *item = launcher_at(entry->launcher);

            if (item != 0) {
                shell_run_app(item->file);
            }
            return;
        }
        if (entry->kind == DOCK_SLOT_APP) {
            int app = entry->app;

            wm.heartbeat[app] = pit_ticks();
            wm_launch_app((enum app_id)app);
        }
        return;
    }
    icon = desktop_icon_at(x, y);
    if (icon >= 0) {
        wm.heartbeat[icon] = pit_ticks();
        wm_launch_app((enum app_id)icon);
    }
}

static void handle_key(const struct key_event *event)
{
    if (event->pressed == 0U) {
        return;
    }
    if (wm.focused >= 0) {
        int app = wm.windows[wm.focused].app;

        if (wm.windows[wm.focused].script >= 0) {
            kby_push_key(kby_app_at(wm.windows[wm.focused].script), event->code,
                         event->pressed != 0U);
            return;
        }
        if (wm.windows[wm.focused].lua >= 0) {
            lua_host_push_key(lua_host_at(wm.windows[wm.focused].lua),
                              event->code, event->pressed != 0U);
            return;
        }
        wm.heartbeat[app] = pit_ticks();
        if (app == APP_TERMINAL) {
            terminal_handle_key(event);
        } else if (app == APP_EDITOR) {
            editor_handle_key(event);
        } else if (app == APP_FILES) {
            int height = wm.windows[wm.focused].height - THEME_TITLEBAR_HEIGHT;
            int count = files_count();
            int row_shift = 0;

            switch (event->code) {
            case KEY_LEFT:
                files_selected--;
                break;
            case KEY_RIGHT:
                files_selected++;
                break;
            case KEY_UP:
                files_selected -= FILES_COLUMNS;
                break;
            case KEY_DOWN:
                files_selected += FILES_COLUMNS;
                break;
            case KEY_PAGE_UP:
                row_shift = -files_visible_rows(height);
                files_selected += row_shift * FILES_COLUMNS;
                break;
            case KEY_PAGE_DOWN:
                row_shift = files_visible_rows(height);
                files_selected += row_shift * FILES_COLUMNS;
                break;
            case KEY_ENTER:
                files_open(files_selected);
                break;
            case KEY_BACKSPACE:
                files_go_up();
                break;
            default:
                break;
            }
            if (count > 0 && files_selected >= count) {
                files_selected = count - 1;
            }
            if (files_selected < 0) {
                files_selected = 0;
            }
            files_clamp(height);
        }
        wm.windows[wm.focused].content_dirty = 1;
        return;
    }
    if (event->code == KEY_ESCAPE) {
        wm.active_menu = -1;
        mark_chrome_dirty();
    }
}

static void mouse_move_by(int delta_x, int delta_y)
{
    int max_x = (int)gfx_width() - 1;
    int max_y = (int)gfx_height() - 1;
    int next_x = wm.mouse_x + delta_x;
    int next_y = wm.mouse_y + delta_y;

    if (next_x < 0) {
        next_x = 0;
    } else if (next_x > max_x) {
        next_x = max_x;
    }
    if (next_y < 0) {
        next_y = 0;
    } else if (next_y > max_y) {
        next_y = max_y;
    }
    wm.mouse_x = next_x;
    wm.mouse_y = next_y;
}

static void process_mouse(const struct mouse_event *event)
{
    int old_x = wm.mouse_x;
    int old_y = wm.mouse_y;
    int released = (int)(wm.mouse_buttons & MOUSE_BUTTON_LEFT) != 0;

    mouse_move_by(event->delta_x, -event->delta_y);
    wm.mouse_buttons = event->buttons;
    if ((event->buttons & MOUSE_BUTTON_LEFT) != 0U) {
        wm.press_x = wm.mouse_x;
        wm.press_y = wm.mouse_y;
        if (released == 0) {
            handle_mouse_press();
        }
    }
    if (wm.dragging != 0U && wm.drag_window >= 0 &&
        (event->buttons & MOUSE_BUTTON_LEFT) != 0U) {
        struct wm_window *window = &wm.windows[wm.drag_window];

        window->x = wm.mouse_x - window->drag_offset_x;
        window->y = wm.mouse_y - window->drag_offset_y;
        /* Leave a few pixels of menubar visible so the title bar is never
         * flush against it, which makes it hard to grab. */
        if (window->y < menubar_height() + 4) {
            window->y = menubar_height() + 4;
        }
        if (window->x > (int)gfx_width() - 60) {
            window->x = (int)gfx_width() - 60;
        }
        if (window->y > (int)gfx_height() - 60) {
            window->y = (int)gfx_height() - 60;
        }
        mark_chrome_dirty();
    }
    if (released && (event->buttons & MOUSE_BUTTON_LEFT) == 0U) {
        wm.dragging = 0U;
        wm.drag_window = -1;
        mark_chrome_dirty();
    }
    if (wm.focused >= 0 && wm.windows[wm.focused].app == APP_FILES) {
        int height = wm.windows[wm.focused].height - THEME_TITLEBAR_HEIGHT;

        if (event->wheel != 0) {
            files_scroll(event->wheel < 0 ? 1 : -1, height);
            wm.windows[wm.focused].content_dirty = 1;
            mark_chrome_dirty();
        }
    } else if (event->wheel < 0) {
        terminal_handle_key(&(struct key_event){
                                .code = KEY_PAGE_UP,
                                .modifiers = 0U,
                                .pressed = 1U
                            });
        mark_chrome_dirty();
    } else if (event->wheel > 0) {
        terminal_handle_key(&(struct key_event){
                                .code = KEY_PAGE_DOWN,
                                .modifiers = 0U,
                                .pressed = 1U
                            });
        mark_chrome_dirty();
    }
    if (old_x != wm.mouse_x || old_y != wm.mouse_y) {
        gfx_damage(old_x - CURSOR_DAMAGE_PAD, old_y - CURSOR_DAMAGE_TOP_PAD,
                   CURSOR_DAMAGE_W, CURSOR_DAMAGE_H);
        gfx_damage(wm.mouse_x - CURSOR_DAMAGE_PAD, wm.mouse_y - CURSOR_DAMAGE_TOP_PAD,
                   CURSOR_DAMAGE_W, CURSOR_DAMAGE_H);
    }
}

static void refresh_chrome(void)
{
    for (int index = 0; index < APP_COUNT; ++index) {
        wm.dock_scale[index] = 256U;
    }
    wm.dock_visible = 1;
    wm.active_menu = -1;
    wm.active_menu_item = -1;
    wm.pressed_menu = -1;
    wm.drag_window = -1;
    wm.dragging = 0;
    wm.focused = -1;
    wm.next_z = 0;
    wm.window_count = 0;
    wm.dock_hover = -1;
    wm.dock_width = 0;
    wm.slot_count = 0;
    wm.frame_count = 0;
    wm.present_count = 0;
    wm.fps = 0;
    wm.composite_count = 0;
    wm.damage_rect_count = 0;
    wm.chrome_dirty = 1U;
    wm.mouse_buttons = 0;
    wm.press_x = 0;
    wm.press_y = 0;
    wm.last_frame_q16 = pit_ticks() * 65536ULL;
    wm.fps_window_start = (uint32_t)pit_ticks();
    wm.fps_window_frames = 0;
    for (int index = 0; index < WM_WINDOW_LIMIT; ++index) {
        wm.windows[index].used = 0;
        wm.windows[index].script = -1;
        wm.windows[index].z = 0;
        wm.windows[index].content_dirty = 0;
    }
    for (int index = 0; index < APP_COUNT; ++index) {
        wm.heartbeat[index] = pit_ticks();
    }
}

const char *wm_wallpaper_error(void)
{
    return wallpaper_error_text;
}

bool wm_set_wallpaper(const char *path)
{
    int length;
    uint8_t *data;
    bool ok;

    wallpaper_error_text = "";
    {
        int index = vfs_open(path);

        if (index < 0) {
            wallpaper_error_text = "no such file";
            return false;
        }
        /* the size from the node rather than by reading to the end, so the
         * whole file is allocated once and read once */
        length = (int)vfs_size(index);
        if (length <= 0) {
            wallpaper_error_text = "the file is empty";
            return false;
        }
        data = (uint8_t *)heap_malloc((size_t)length);
        if (data == 0) {
            wallpaper_error_text = "not enough memory to read the file";
            return false;
        }
        if (vfs_read(path, (char *)data, (uint32_t)length) != length) {
            heap_free(data);
            wallpaper_error_text = "the file could not be read";
            return false;
        }
    }
    ok = png_decode(data, (size_t)length, wm.wallpaper);
    heap_free(data);
    if (!ok) {
        wallpaper_error_text = png_error();
        /* the background is left as it was rather than cleared, so a file that
         * fails to decode leaves a working desktop instead of a blank one */
        return false;
    }
    /* The whole screen, not just the chrome.
     *
     * The background sits behind every window, so changing it means the region
     * it occupies has to be repainted rather than only the title bars and the
     * dock that mark_chrome_dirty covers.  Marking only the chrome left the
     * old background on the screen with the new one decoded in memory behind
     * it, which looks exactly like the command having done nothing. */
    wm.chrome_dirty = 1U;
    for (int index = 0; index < WM_WINDOW_LIMIT; ++index) {
        if (wm.windows[index].used != 0) {
            wm.windows[index].content_dirty = 1U;
        }
    }
    gfx_damage(0, 0, (int)gfx_width(), (int)gfx_height());
    return true;
}

void wm_init(void)
{
    uint32_t width = gfx_width();
    uint32_t height = gfx_height();

    wm.back = gfx_backbuffer();
    wallpaper_pixels = (uint32_t *)gfx_alloc(width * height * 4U);
    if (wallpaper_pixels == NULL) {
        panic("wm: surface allocation failed");
    }
    wallpaper_surface.pixels = wallpaper_pixels;
    wallpaper_surface.width = width;
    wallpaper_surface.height = height;
    wallpaper_surface.pitch_pixels = width;
    wallpaper_surface.origin_x = 0;
    wallpaper_surface.origin_y = 0;
    wm.wallpaper = &wallpaper_surface;
    wm.mouse_x = (int)width / 2;
    wm.mouse_y = (int)height / 2;
    build_wallpaper();
    refresh_chrome();
    apps_init();
    shell_init();
    gfx_damage_all();
    full_recompose();
    gfx_present();
    wm.present_count++;
}

static void service_scripts(void)
{
    for (int index = 0; index < WM_WINDOW_LIMIT; ++index) {
        struct kby_app *app;

        if (wm.windows[index].used == 0 || wm.windows[index].script < 0) {
            continue;
        }
        app = kby_app_at(wm.windows[index].script);
        if (app == 0) {
            continue;
        }
        kby_set_mouse(app, wm.mouse_x - wm.windows[index].x,
                      wm.mouse_y - wm.windows[index].y - THEME_TITLEBAR_HEIGHT,
                      (wm.mouse_buttons & MOUSE_BUTTON_LEFT) != 0U);
        if (kby_app_loaded(app) == false) {
            /* halted programs keep their final frame on screen */
            continue;
        }
        kby_clear_display(app);
        kby_run(app, KBY_BUDGET);
        if (kby_app_loaded(app)) {
            wm.windows[index].content_dirty = 1;
        }
    }
}

/* Steps the Lua hosts once per composited frame rather than once per service
 * call.  wm_service runs at the timer rate, and a script that repaints on
 * every one of those is both wasteful and enough to hold the whole compositor
 * down to the frame rate. */
static void service_lua(void)
{
    for (int index = 0; index < WM_WINDOW_LIMIT; ++index) {
        struct lua_host *host;

        if (wm.windows[index].used == 0 || wm.windows[index].lua < 0) {
            continue;
        }
        host = lua_host_at(wm.windows[index].lua);
        if (host == 0) {
            continue;
        }
        lua_host_set_mouse(host, wm.mouse_x - wm.windows[index].x,
                           wm.mouse_y - wm.windows[index].y -
                               THEME_TITLEBAR_HEIGHT,
                           (wm.mouse_buttons & MOUSE_BUTTON_LEFT) != 0U);
        if (lua_host_loaded(host) == false) {
            /* a failed script keeps its last good frame on screen */
            continue;
        }
        lua_host_service(host);
        /* only when the display list actually changed: a static window that
         * repaints identically every tick would otherwise force a redraw, and
         * for a picture that means decoding every pixel again for nothing */
        if (lua_host_list_changed(host)) {
            wm.windows[index].content_dirty = 1;
        }
    }
}

void wm_service(void)
{
    struct mouse_event mouse_event;
    struct key_event key_event;
    uint64_t now;
    int previous_menu = wm.active_menu;
    int previous_item = wm.active_menu_item;

    if (wm.ready == 0U) {
        return;
    }
    service_scripts();
    while (input_poll_mouse(&mouse_event)) {
        process_mouse(&mouse_event);
    }
    while (input_poll_key(&key_event)) {
        handle_key(&key_event);
    }
    if ((wm.mouse_buttons & MOUSE_BUTTON_LEFT) != 0U) {
        int item = menu_item_at(wm.mouse_x, wm.mouse_y);

        if (item != wm.active_menu_item) {
            wm.active_menu_item = item;
            if (wm.active_menu >= 0) {
                mark_chrome_dirty();
            }
        }
    } else if (wm.active_menu >= 0) {
        int item = menu_item_at(wm.mouse_x, wm.mouse_y);

        if (item != wm.active_menu_item) {
            wm.active_menu_item = item;
            mark_chrome_dirty();
        }
    }
    {
        int item = wm.active_menu >= 0
                       ? menu_item_at(wm.mouse_x, wm.mouse_y)
                       : -1;

        if (item != wm.active_menu_item) {
            wm.active_menu_item = item;
            mark_chrome_dirty();
        }
    }
    if (previous_menu != wm.active_menu ||
        previous_item != wm.active_menu_item) {
        mark_chrome_dirty();
    }
    layout_dock();
    update_dock_animation();

    for (int index = 0; index < WM_WINDOW_LIMIT; ++index) {
        if (wm.windows[index].used != 0 && wm.windows[index].content_dirty != 0) {
            damage_window(index);
        }
    }
    {
        uint64_t phase = (pit_ticks() % 1060U);

        if (wm.focused >= 0) {
            int app = wm.windows[wm.focused].app;

            if (app == APP_TERMINAL || app == APP_EDITOR) {
                static uint8_t was_visible;

                if ((phase < 530U) != (was_visible != 0U)) {
                    was_visible = (uint8_t)(phase < 530U ? 1U : 0U);
                    mark_chrome_dirty();
                }
            }
        }
    }
    {
        static uint32_t last_second;

        uint32_t second = (uint32_t)(pit_ticks() / 1000U);

        if (second != last_second) {
            last_second = second;
            gfx_damage(0, 0, (int)gfx_width(), menubar_height());
        }
    }

    now = pit_ticks() * 65536ULL;
    if (now - wm.last_frame_q16 < FRAME_INTERVAL_Q16) {
        return;
    }
    wm.last_frame_q16 += FRAME_INTERVAL_Q16;
    service_lua();
    if (now - wm.last_frame_q16 > FRAME_INTERVAL_Q16 * 4ULL) {
        wm.last_frame_q16 = now;
    }
    wm.frame_count++;
    wm.fps_window_frames++;
    {
        uint32_t elapsed = (uint32_t)pit_ticks() - wm.fps_window_start;

        if (elapsed >= 1000U) {
            wm.fps = wm.fps_window_frames * 1000U / elapsed;
            wm.fps_window_frames = 0;
            wm.fps_window_start = (uint32_t)pit_ticks();
        }
    }
    repaint_damage();
    gfx_present();
    wm.present_count++;
    if (shell_reboot_pending()) {
        shell_machine_reboot();
    }
    if (shell_halt_pending()) {
        shell_machine_halt();
    }
}

void wm_run_boot_animation(void)
{
    /* Deliberately plain: a black screen, the K, and a bar that fills.  The
     * previous version had a gradient, a ring that swept, a light sweep and
     * three lines of text, which was a lot of motion for something the eye
     * only passes once on the way to the desktop. */
    static const uint32_t black = 0xFF000000U;
    static const uint32_t white = 0xFFFFFFFFU;
    static const uint32_t track = 0xFF282828U;
    int center_x = (int)gfx_width() / 2;
    int center_y = (int)gfx_height() / 2;
    int bar_width = 320;
    int bar_height = 6;
    int bar_x = center_x - bar_width / 2;
    int bar_y = center_y + 120;

    for (int frame = 0; frame < BOOT_ANIMATION_FRAMES; ++frame) {
        int progress = frame * 100 / (BOOT_ANIMATION_FRAMES - 1);

        gfx_fill(wm.back, 0, 0, (int)gfx_width(), (int)gfx_height(), black);
        font_draw_centered(wm.back, center_x, center_y + 20, "K", white, 8);

        gfx_fill(wm.back, bar_x, bar_y, bar_width, bar_height, track);
        gfx_fill(wm.back, bar_x, bar_y, bar_width * progress / 100, bar_height,
                 white);

        gfx_damage_all();
        gfx_present();

        animation_tick++;
        {
            uint64_t target = wm.last_frame_q16 + FRAME_INTERVAL_Q16;

            while (pit_ticks() * 65536ULL < target) {
                __asm__ volatile("hlt");
            }
            wm.last_frame_q16 = target;
        }
    }
    wm.frame_count += BOOT_ANIMATION_FRAMES;
    mark_chrome_dirty();
}
