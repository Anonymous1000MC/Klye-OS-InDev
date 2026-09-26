#ifndef KLYE_GUI_H
#define KLYE_GUI_H

#include <stdbool.h>
#include <stdint.h>

#define CONSOLE_COLOR_OK 0xFF7DE88FU
#define CONSOLE_COLOR_WARN 0xFFFFC46BU
#define CONSOLE_COLOR_ACCENT 0xFF6FD3FFU

bool gui_init(uint64_t address, uint32_t pitch, uint32_t width,
              uint32_t height, uint8_t bpp, uint8_t framebuffer_type);
void gui_service(void);
void gui_mouse_irq(void);
void gui_keyboard_irq(void);
void gui_mouse_packet(int16_t x_delta, int16_t y_delta, uint8_t buttons);
uint8_t gui_mouse_config(void);
bool gui_mouse_ready(void);
uint32_t gui_mouse_packets(void);
uint32_t gui_key_scancodes(void);
void console_init(void);
void console_write(const char *text);
void console_clear(void);
void console_paint(void);
void console_flush(void);
void console_status(const char *label, const char *status, uint32_t color);
void console_number(const char *label, uint64_t value, const char *suffix);
void gui_init_input(void);
void gui_run_boot_animation(void);
void gui_cli_task(uint64_t argument);
void gui_compositor_task(uint64_t argument);
void gui_post_note(const char *text);

#endif
