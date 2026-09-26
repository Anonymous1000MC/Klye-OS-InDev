#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "apps.h"
#include "font.h"
#include "gfx.h"
#include "gui.h"
#include "input.h"
#include "shell.h"
#include "theme.h"
#include "wm.h"

#define CONSOLE_COLS 92
#define CONSOLE_ROWS 40
#define CONSOLE_PAD 26

struct console_cell {
    char glyph;
    uint8_t color_id;
};

static struct console_cell console_grid[CONSOLE_ROWS][CONSOLE_COLS];
static uint32_t console_colour[4];
static int console_col;
static int console_row;
static bool console_dirty;
static bool console_ready;
static bool console_rendered;
static int console_origin_x;
static int console_origin_y;

static void console_colour_id(uint32_t color)
{
    for (uint32_t index = 0; index < 4U; ++index) {
        if (console_colour[index] == color) {
            return;
        }
    }
}

static void console_put(char glyph, uint32_t color)
{
    if (console_col >= CONSOLE_COLS) {
        console_col = 0;
        console_row++;
    }
    if (console_row >= CONSOLE_ROWS) {
        for (int row = 0; row < CONSOLE_ROWS - 1; ++row) {
            for (int column = 0; column < CONSOLE_COLS; ++column) {
                console_grid[row][column] = console_grid[row + 1][column];
            }
        }
        for (int column = 0; column < CONSOLE_COLS; ++column) {
            console_grid[CONSOLE_ROWS - 1][column].glyph = ' ';
            console_grid[CONSOLE_ROWS - 1][column].color_id = 0;
        }
        console_row = CONSOLE_ROWS - 1;
    }
    console_grid[console_row][console_col].glyph = glyph;
    console_grid[console_row][console_col].color_id = (uint8_t)color;
    console_col++;
    console_dirty = true;
}

bool gui_init(uint64_t address, uint32_t pitch, uint32_t width,
              uint32_t height, uint8_t bpp, uint8_t framebuffer_type)
{
    if (address == 0 || pitch < width * 4U || bpp != 32U ||
        framebuffer_type != 1U || width < 640U || height < 480U) {
        return false;
    }
    if (width > 1920U || height > 1080U) {
        return false;
    }
    gfx_init((uint32_t)(uintptr_t)address, pitch, width, height);
    if (gfx_backbuffer()->pixels == 0) {
        return false;
    }
    return true;
}

void console_init(void)
{
    console_colour[0] = THEME_CONSOLE_TEXT;
    console_colour[1] = CONSOLE_COLOR_OK;
    console_colour[2] = CONSOLE_COLOR_WARN;
    console_colour[3] = CONSOLE_COLOR_ACCENT;
    console_origin_x = (int)(gfx_width() / 2U) - (CONSOLE_COLS * FONT_ADVANCE) / 2;
    console_origin_y = (int)(gfx_height() / 2U) -
                       (CONSOLE_ROWS * FONT_LINE_HEIGHT) / 2;
    console_ready = true;
    console_clear();
    console_paint();
}

void console_write(const char *text)
{
    if (!console_ready) {
        return;
    }
    while (*text != '\0') {
        if (*text == '\n') {
            console_col = 0;
            console_row++;
            while (console_row >= CONSOLE_ROWS) {
                for (int row = 0; row < CONSOLE_ROWS - 1; ++row) {
                    for (int column = 0; column < CONSOLE_COLS; ++column) {
                        console_grid[row][column] =
                            console_grid[row + 1][column];
                    }
                }
                for (int column = 0; column < CONSOLE_COLS; ++column) {
                    console_grid[CONSOLE_ROWS - 1][column].glyph = ' ';
                    console_grid[CONSOLE_ROWS - 1][column].color_id = 0;
                }
                console_row = CONSOLE_ROWS - 1;
            }
            text++;
            continue;
        }
        if (*text == '\r') {
            console_col = 0;
            text++;
            continue;
        }
        console_put(*text, 0);
        text++;
    }
}

void console_clear(void)
{
    for (int row = 0; row < CONSOLE_ROWS; ++row) {
        for (int column = 0; column < CONSOLE_COLS; ++column) {
            console_grid[row][column].glyph = ' ';
            console_grid[row][column].color_id = 0;
        }
    }
    console_col = 0;
    console_row = 0;
    console_dirty = true;
}

void console_number(const char *label, uint64_t value, const char *suffix)
{
    char digits[24];

    console_write(label);
    if (value == 0) {
        console_put('0', 0);
    }
    {
        uint32_t length = 0;

        while (value != 0 && length < sizeof(digits) - 1U) {
            digits[length++] = (char)('0' + (uint32_t)(value % 10U));
            value /= 10U;
        }
        while (length > 0) {
            console_put(digits[--length], 0);
        }
    }
    console_write(suffix);
}

void console_status(const char *label, const char *status, uint32_t color)
{
    uint32_t identifier = 1;

    console_colour_id(color);
    for (uint32_t index = 1; index < 4U; ++index) {
        if (console_colour[index] == color) {
            identifier = index;
        }
    }
    console_write("  [");
    for (const char *cursor = status; *cursor != '\0'; ++cursor) {
        console_put(*cursor, identifier);
    }
    console_write("] ");
    console_write(label);
}

void console_paint(void)
{
    struct gfx_surface *back = gfx_backbuffer();
    char glyph[2];
    int panel_width = CONSOLE_COLS * FONT_ADVANCE + CONSOLE_PAD * 2;
    int panel_height = CONSOLE_ROWS * FONT_LINE_HEIGHT + CONSOLE_PAD * 2;

    if (!console_ready) {
        return;
    }
    gfx_gradient_v(back, 0, 0, (int)gfx_width(), (int)gfx_height(),
                   THEME_WALLPAPER_TOP, THEME_WALLPAPER_BOTTOM);
    gfx_rounded_shadow(back, console_origin_x - CONSOLE_PAD,
                       console_origin_y - CONSOLE_PAD, panel_width,
                       panel_height, 14, 26, 8, THEME_SHADOW_COLOR, 130U);
    gfx_rounded_rect_alpha(back, console_origin_x - CONSOLE_PAD,
                           console_origin_y - CONSOLE_PAD, panel_width,
                           panel_height, 14, THEME_CONSOLE_BG, 244U);
    gfx_rounded_border_alpha(back, console_origin_x - CONSOLE_PAD,
                             console_origin_y - CONSOLE_PAD, panel_width,
                             panel_height, 14, 1,
                             PIXEL_RGB(0xFF, 0xFF, 0xFF), 60U);
    for (int row = 0; row < CONSOLE_ROWS; ++row) {
        for (int column = 0; column < CONSOLE_COLS; ++column) {
            const struct console_cell *cell = &console_grid[row][column];
            uint32_t index = cell->color_id;

            if (index > 3U) {
                index = 0;
            }
            if (cell->glyph == ' ') {
                continue;
            }
            glyph[0] = cell->glyph;
            glyph[1] = '\0';
            font_draw(back, console_origin_x + column * FONT_ADVANCE,
                      console_origin_y + row * FONT_LINE_HEIGHT + 1, glyph,
                      console_colour[index], 1);
        }
    }
    console_dirty = false;
    console_rendered = true;
}

void console_flush(void)
{
    if (!console_ready) {
        return;
    }
    if (console_dirty) {
        console_paint();
    }
    gfx_damage_all();
    gfx_present();
}

void gui_service(void)
{
    wm_service();
}

void gui_mouse_irq(void)
{
    input_mouse_irq();
}

void gui_keyboard_irq(void)
{
    input_keyboard_irq();
}

void gui_mouse_packet(int16_t x_delta, int16_t y_delta, uint8_t buttons)
{
    input_handle_mouse_packet(x_delta, y_delta, buttons);
}

void gui_init_input(void)
{
    input_init();
}

uint8_t gui_mouse_config(void)
{
    return input_mouse_config();
}

bool gui_mouse_ready(void)
{
    return input_mouse_ready();
}

uint32_t gui_mouse_packets(void)
{
    return input_mouse_packets();
}

uint32_t gui_key_scancodes(void)
{
    return input_key_scancodes();
}

void gui_run_boot_animation(void)
{
    wm_run_boot_animation();
}

static void cli_render(void)
{
    int count = terminal_line_count();

    console_clear();
    for (int index = 0; index < count; ++index) {
        console_write(terminal_line_text(index));
        console_write("\n");
    }
    console_flush();
}

void gui_cli_task(uint64_t argument)
{
    int last_lines = -1;
    int last_column = -1;

    (void)argument;
    for (;;) {
        struct key_event event;
        int lines = terminal_line_count();
        int column = terminal_cursor_column();

        while (input_poll_key(&event)) {
            terminal_handle_key(&event);
            lines = terminal_line_count();
            column = terminal_cursor_column();
        }
        if (lines != last_lines || column != last_column) {
            last_lines = lines;
            last_column = column;
            cli_render();
        }
        if (shell_reboot_pending()) {
            shell_machine_reboot();
        }
        if (shell_halt_pending()) {
            shell_machine_halt();
        }
        __asm__ volatile("hlt");
    }
}

void gui_compositor_task(uint64_t argument)
{
    (void)argument;
    for (;;) {
        wm_service();
        __asm__ volatile("hlt");
    }
}

void gui_post_note(const char *text)
{
    console_write(text);
    console_flush();
}
