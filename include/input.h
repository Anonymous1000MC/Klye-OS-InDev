#ifndef KLYE_INPUT_H
#define KLYE_INPUT_H

#include <stdbool.h>
#include <stdint.h>

#define KEY_NONE 0U
#define KEY_CHAR_BASE 32U
#define KEY_CHAR_LAST 126U
#define KEY_NAMED_BASE 128U

enum key_code {
    KEY_ESCAPE = KEY_NAMED_BASE,
    KEY_ENTER,
    KEY_TAB,
    KEY_BACKSPACE,
    KEY_SPACE,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_UP,
    KEY_DOWN,
    KEY_HOME,
    KEY_END,
    KEY_PAGE_UP,
    KEY_PAGE_DOWN,
    KEY_INSERT,
    KEY_DELETE,
    KEY_F1,
    KEY_F2,
    KEY_F3,
    KEY_F4,
    KEY_F5,
    KEY_F6,
    KEY_F7,
    KEY_F8,
    KEY_F9,
    KEY_F10,
    KEY_F11,
    KEY_F12,
    KEY_LEFT_SHIFT,
    KEY_RIGHT_SHIFT,
    KEY_LEFT_CTRL,
    KEY_RIGHT_CTRL,
    KEY_LEFT_ALT,
    KEY_RIGHT_ALT,
    KEY_CAPS_LOCK,
    KEY_CODE_COUNT
};

#define MOD_SHIFT 0x01U
#define MOD_CTRL 0x02U
#define MOD_ALT 0x04U
#define MOD_CAPS 0x08U

#define MOUSE_BUTTON_LEFT 0x01U
#define MOUSE_BUTTON_RIGHT 0x02U
#define MOUSE_BUTTON_MIDDLE 0x04U

struct key_event {
    uint16_t code;
    uint16_t modifiers;
    uint8_t pressed;
};

struct mouse_event {
    int16_t delta_x;
    int16_t delta_y;
    int8_t wheel;
    uint8_t buttons;
};

void input_init(void);
bool input_poll_key(struct key_event *event);
bool input_poll_mouse(struct mouse_event *event);
char input_key_ascii(const struct key_event *event);
bool input_key_is_modifier(uint16_t code);
bool input_key_is_named(uint16_t code);
const char *input_key_name(uint16_t code);

void input_handle_mouse_packet(int16_t delta_x, int16_t delta_y,
                              uint8_t buttons);

/* As above, with the wheel: a signed count of detents, positive away from the
 * user. */
void input_handle_mouse_wheel(int16_t delta_x, int16_t delta_y, int8_t wheel,
                              uint8_t buttons);
void input_handle_scancode(uint8_t scancode);

void input_keyboard_irq(void);
void input_mouse_irq(void);
uint8_t input_mouse_config(void);
bool input_mouse_ready(void);
uint32_t input_mouse_packets(void);
uint32_t input_key_scancodes(void);
uint32_t input_key_events(void);
uint32_t input_dropped_keys(void);
uint32_t input_dropped_mouse(void);

#endif
