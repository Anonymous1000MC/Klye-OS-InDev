#ifndef KLYE_APPS_H
#define KLYE_APPS_H

#include <stdbool.h>
#include <stdint.h>

#include "gfx.h"
#include "input.h"

enum app_id {
    APP_TERMINAL = 0,
    APP_EDITOR,
    APP_FILES,
    APP_SETTINGS,
    APP_BROWSER,
    APP_ABOUT,
    APP_COUNT
};

#define TERMINAL_MAX_LINES 400
#define TERMINAL_MAX_COLS 108
#define EDITOR_MAX_LINES 400
#define EDITOR_MAX_COLS 96
#define EDITOR_NAME_MAX 32

struct terminal_view {
    int first_line;
    int rows;
    int columns;
};

struct editor_view {
    int first_line;
    int rows;
};

void apps_init(void);
const char *app_name(enum app_id app);
const char *app_title(enum app_id app);
uint32_t app_accent(enum app_id app);
bool app_is_open(enum app_id app);
int app_open_count(void);
void app_open(enum app_id app);
void app_close(enum app_id app);
void app_toggle(enum app_id app);
void apps_close_all(void);
int app_from_name(const char *name);
void app_draw_icon(struct gfx_surface *surface, enum app_id app, int x, int y,
                   int size);

void terminal_reset(void);
void terminal_puts(const char *text);
void terminal_error(void);
void terminal_printf_number(uint64_t value);
void terminal_clear(void);
void terminal_handle_key(const struct key_event *event);
void terminal_draw(struct gfx_surface *surface, int x, int y, int width,
                   int height);
int terminal_line_count(void);
int terminal_total_lines(void);
int terminal_cursor_column(void);
int terminal_is_prompt(void);
const char *terminal_line_text(int index);
int terminal_line_length(int index);
bool terminal_line_is_prompt(int index);
bool terminal_has_output(void);

void editor_reset(void);
void editor_handle_key(const struct key_event *event);
void editor_draw(struct gfx_surface *surface, int x, int y, int width,
                 int height);
void editor_load(const char *name, const char *body);
int editor_save(void);
const char *editor_name(void);
int editor_line_count(void);
int editor_cursor_line(void);
int editor_cursor_column(void);
bool editor_modified(void);
bool editor_saved(void);
int editor_word_count(void);
int editor_char_count(void);
const char *editor_line_text(int index);
int editor_line_length(int index);


#endif
