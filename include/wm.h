#ifndef KLYE_WM_H
#define KLYE_WM_H

#include <stdbool.h>
#include <stdint.h>

#include "apps.h"
#include "gfx.h"

#define WM_WINDOW_LIMIT 8

enum wm_action {
    WM_ACTION_NONE = 0,
    WM_ACTION_ABOUT,
    WM_ACTION_SETTINGS,
    WM_ACTION_NEW_TERMINAL,
    WM_ACTION_NEW_EDITOR,
    WM_ACTION_OPEN_FILES,
    WM_ACTION_CLOSE_WINDOW,
    WM_ACTION_MINIMIZE_WINDOW,
    WM_ACTION_ZOOM_WINDOW,
    WM_ACTION_TOGGLE_DOCK,
    WM_ACTION_SHOW_HELP,
    WM_ACTION_REBOOT,
    WM_ACTION_SHUTDOWN,
    WM_ACTION_CLEAR_TERMINAL,
    WM_ACTION_FOCUS_NEXT,
    /* Settings panel: move between rows, and change the selected value. */
    WM_ACTION_SETTING_UP,
    WM_ACTION_SETTING_DOWN,
    WM_ACTION_SETTING_DECREASE,
    WM_ACTION_SETTING_INCREASE,
    WM_ACTION_SETTINGS_CATEGORY_UP,
    WM_ACTION_SETTINGS_CATEGORY_DOWN,
    WM_ACTION_COUNT
};

void wm_init(void);
void wm_service(void);

/* Set the desktop background from a PNG on the filesystem, scaled to cover the
 * screen.  Falls back to the built in background if the file cannot be read or
 * decoded, so a bad path leaves a working desktop rather than a blank one.
 * Returns false with a reason in wm_wallpaper_error(). */
bool wm_set_wallpaper(const char *path);
const char *wm_wallpaper_error(void);
void wm_launch_app(enum app_id app);
void wm_launch_lua(int host_index, const char *title, int width, int height);
void wm_launch_script(int script_index, const char *title, int width,
                      int height);
void wm_close_focused(void);
void wm_minimize_focused(void);
void wm_run_action(enum wm_action action);
bool wm_menu_is_open(void);
bool wm_dock_visible(void);
void wm_toggle_dock(void);
int wm_focused_window(void);
int wm_open_window_count(void);
uint64_t wm_app_heartbeat(enum app_id app);
uint32_t wm_fps(void);
uint32_t wm_frame_count(void);
uint32_t wm_present_count(void);
uint64_t wm_uptime_ticks(void);
void wm_run_boot_animation(void);
void wm_set_ready(bool ready);
void wm_benchmark(uint32_t iterations);
void wm_benchmark_incremental(uint32_t iterations);

#endif
