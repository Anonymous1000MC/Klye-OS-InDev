#include <stdbool.h>
#include <stdint.h>

#include "input.h"
#include "io.h"
#include "kernel.h"

#define PS2_STATUS_OUTPUT_FULL 0x01U
#define PS2_STATUS_MOUSE 0x20U
#define PS2_STATUS_INPUT_FULL 0x02U
#define PS2_CONFIG_MOUSE_IRQ 0x02U
#define PS2_WAIT_ATTEMPTS 100000U
#define PS2_ACK_ATTEMPTS 20000U
#define PS2_ACK_SETTLE 64U
#define PS2_COMMAND_RETRIES 4U
#define PS2_IRQ_DRAIN_LIMIT 32U
#define PS2_MOUSE_PROBE_ROUNDS 4000U
#define PS2_MOUSE_PROBE_SPINS 2000U
#define PS2_ACK_OK 0xFAU
#define SCANCODE_EXTENDED 0xE0U
#define SCANCODE_RELEASE 0xF0U
#define KEY_QUEUE_SIZE 256
#define MOUSE_QUEUE_SIZE 128

static uint8_t ps2_config_byte;
static bool ps2_mouse_configured;
static volatile uint32_t mouse_packet_count;
volatile uint32_t mouse_irq_calls;
volatile uint32_t mouse_irq_no_data;
volatile uint32_t mouse_irq_not_mouse;
volatile uint32_t mouse_irq_bytes;
volatile uint32_t keyboard_irq_calls;
volatile uint32_t keyboard_irq_bytes;
volatile uint32_t keyboard_irq_empty;
volatile uint32_t keyboard_irq_aux;
volatile uint32_t mouse_last_status;
volatile uint32_t keyboard_last_status;
volatile uint8_t ps2_ack_selftest;
volatile uint8_t ps2_ack_f6;
volatile uint8_t ps2_ack_f4;
volatile uint8_t ps2_status_probe;
volatile bool ps2_mouse_reporting;
static volatile uint32_t key_scancode_count;
static volatile uint32_t key_event_count;
static volatile uint32_t dropped_key_count;
static volatile uint32_t dropped_mouse_count;

static struct key_event key_queue[KEY_QUEUE_SIZE];
static volatile uint32_t key_read;
static volatile uint32_t key_write;
static struct mouse_event mouse_queue[MOUSE_QUEUE_SIZE];
static volatile uint32_t mouse_read;
static volatile uint32_t mouse_write;
static uint16_t modifiers;
static bool extended_pending;
static bool release_pending;
static uint8_t mouse_packet[4];
/* True once the mouse has agreed to the wheel device id, which means its
 * packets are four bytes.  Until then they are three. */
static bool ps2_mouse_wheel;
static uint8_t mouse_packet_index;
static uint8_t previous_buttons;
static bool mouse_liveness_reported;

static const uint16_t scancode_map[128] = {
    [0x01] = KEY_ESCAPE,
    [0x02] = '1',  [0x03] = '2',  [0x04] = '3',  [0x05] = '4',
    [0x06] = '5',  [0x07] = '6',  [0x08] = '7',  [0x09] = '8',
    [0x0A] = '9',  [0x0B] = '0',  [0x0C] = '-',  [0x0D] = '=',
    [0x0E] = KEY_BACKSPACE,
    [0x0F] = KEY_TAB,
    [0x10] = 'q',  [0x11] = 'w',  [0x12] = 'e',  [0x13] = 'r',
    [0x14] = 't',  [0x15] = 'y',  [0x16] = 'u',  [0x17] = 'i',
    [0x18] = 'o',  [0x19] = 'p',  [0x1A] = '[',  [0x1B] = ']',
    [0x1C] = KEY_ENTER,
    [0x1D] = KEY_LEFT_CTRL,
    [0x1E] = 'a',  [0x1F] = 's',  [0x20] = 'd',  [0x21] = 'f',
    [0x22] = 'g',  [0x23] = 'h',  [0x24] = 'j',  [0x25] = 'k',
    [0x26] = 'l',  [0x27] = ';',  [0x28] = '\'', [0x29] = '`',
    [0x2A] = KEY_LEFT_SHIFT,
    [0x2B] = '\\',
    [0x2C] = 'z',  [0x2D] = 'x',  [0x2E] = 'c',  [0x2F] = 'v',
    [0x30] = 'b',  [0x31] = 'n',  [0x32] = 'm',
    [0x33] = ',',  [0x34] = '.',  [0x35] = '/',
    [0x36] = KEY_RIGHT_SHIFT,
    [0x37] = '*',
    [0x38] = KEY_LEFT_ALT,
    [0x39] = KEY_SPACE,
    [0x3A] = KEY_CAPS_LOCK,
    [0x3B] = KEY_F1,  [0x3C] = KEY_F2,  [0x3D] = KEY_F3,
    [0x3E] = KEY_F4,  [0x3F] = KEY_F5,  [0x40] = KEY_F6,
    [0x41] = KEY_F7,  [0x42] = KEY_F8,  [0x43] = KEY_F9,
    [0x44] = KEY_F10,
    [0x47] = KEY_HOME,
    [0x48] = KEY_UP,
    [0x49] = KEY_PAGE_UP,
    [0x4A] = '-',
    [0x4B] = KEY_LEFT,
    [0x4D] = KEY_RIGHT,
    [0x4E] = '+',
    [0x4F] = KEY_END,
    [0x50] = KEY_DOWN,
    [0x51] = KEY_PAGE_DOWN,
    [0x52] = KEY_INSERT,
    [0x53] = KEY_DELETE,
    [0x57] = KEY_F11,
    [0x58] = KEY_F12
};

static const char shifted_map[128] = {
    [' '] = ' ',
    ['!'] = '!',
    ['"'] = '"',
    ['#'] = '#',
    ['$'] = '$',
    ['%'] = '%',
    ['&'] = '&',
    ['\''] = '\'',
    ['('] = '(',
    [')'] = ')',
    ['*'] = '*',
    ['+'] = '+',
    [','] = '<',
    ['-'] = '_',
    ['.'] = '>',
    ['/'] = '?',
    ['0'] = ')',
    ['1'] = '!',
    ['2'] = '@',
    ['3'] = '#',
    ['4'] = '$',
    ['5'] = '%',
    ['6'] = '^',
    ['7'] = '&',
    ['8'] = '*',
    ['9'] = '(',
    [':'] = ':',
    [';'] = ':',
    ['<'] = '<',
    ['='] = '+',
    ['>'] = '>',
    ['?'] = '?',
    ['@'] = '@',
    ['A'] = 'A',
    ['B'] = 'B',
    ['C'] = 'C',
    ['D'] = 'D',
    ['E'] = 'E',
    ['F'] = 'F',
    ['G'] = 'G',
    ['H'] = 'H',
    ['I'] = 'I',
    ['J'] = 'J',
    ['K'] = 'K',
    ['L'] = 'L',
    ['M'] = 'M',
    ['N'] = 'N',
    ['O'] = 'O',
    ['P'] = 'P',
    ['Q'] = 'Q',
    ['R'] = 'R',
    ['S'] = 'S',
    ['T'] = 'T',
    ['U'] = 'U',
    ['V'] = 'V',
    ['W'] = 'W',
    ['X'] = 'X',
    ['Y'] = 'Y',
    ['Z'] = 'Z',
    ['['] = '{',
    ['\\'] = '|',
    [']'] = '}',
    ['^'] = '^',
    ['_'] = '_',
    ['`'] = '~',
    ['a'] = 'A',
    ['b'] = 'B',
    ['c'] = 'C',
    ['d'] = 'D',
    ['e'] = 'E',
    ['f'] = 'F',
    ['g'] = 'G',
    ['h'] = 'H',
    ['i'] = 'I',
    ['j'] = 'J',
    ['k'] = 'K',
    ['l'] = 'L',
    ['m'] = 'M',
    ['n'] = 'N',
    ['o'] = 'O',
    ['p'] = 'P',
    ['q'] = 'Q',
    ['r'] = 'R',
    ['s'] = 'S',
    ['t'] = 'T',
    ['u'] = 'U',
    ['v'] = 'V',
    ['w'] = 'W',
    ['x'] = 'X',
    ['y'] = 'Y',
    ['z'] = 'Z',
    ['{'] = '{',
    ['|'] = '|',
    ['}'] = '}',
    ['~'] = '~',
};

static const char *const key_names[KEY_CODE_COUNT] = {
    "esc", "enter", "tab", "backspace", "space", "left", "right",
    "up", "down", "home", "end", "pageup", "pagedown", "insert", "delete",
    "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11",
    "f12", "lshift", "rshift", "lctrl", "rctrl", "lalt", "ralt", "caps"
};

static bool wait_write(void)
{
    for (uint32_t attempt = 0; attempt < PS2_WAIT_ATTEMPTS; ++attempt) {
        if ((inb(0x64) & PS2_STATUS_INPUT_FULL) == 0U) {
            return true;
        }
    }
    return false;
}

static void delay_spins(uint32_t count)
{
    volatile uint32_t sink = 0;

    for (uint32_t index = 0; index < count; ++index) {
        sink = sink + 1U;
    }
}

static void drain_output(void)
{
    while ((inb(0x64) & PS2_STATUS_OUTPUT_FULL) != 0U) {
        (void)inb(0x60);
    }
}

static uint8_t read_response(bool prefer_mouse)
{
    uint8_t fallback = 0;
    bool have = false;

    for (uint32_t attempt = 0; attempt < PS2_ACK_ATTEMPTS; ++attempt) {
        uint8_t status = inb(0x64);

        if ((status & PS2_STATUS_OUTPUT_FULL) == 0U) {
            if (have) {
                break;
            }
            delay_spins(PS2_ACK_SETTLE);
            continue;
        }
        {
            uint8_t value = inb(0x60);

            if (prefer_mouse && (status & PS2_STATUS_MOUSE) != 0U) {
                return value;
            }
            if (!have) {
                fallback = value;
                have = true;
            }
        }
    }
    return fallback;
}

static uint8_t send_command(uint8_t command)
{
    for (uint32_t retry = 0; retry < PS2_COMMAND_RETRIES; ++retry) {
        uint8_t result;

        drain_output();
        if (!wait_write()) {
            continue;
        }
        outb(0x64, command);
        result = read_response(false);
        drain_output();
        if (result != 0U) {
            return result;
        }
    }
    return 0U;
}

static void write_command_byte(uint8_t value)
{
    for (uint32_t retry = 0; retry < PS2_COMMAND_RETRIES; ++retry) {
        drain_output();
        if (!wait_write()) {
            continue;
        }
        outb(0x64, 0x60);
        if (!wait_write()) {
            continue;
        }
        outb(0x60, value);
        return;
    }
}

static uint8_t send_aux_command(uint8_t command)
{
    for (uint32_t retry = 0; retry < PS2_COMMAND_RETRIES; ++retry) {
        uint8_t result;

        drain_output();
        if (!wait_write()) {
            continue;
        }
        outb(0x64, 0xD4);
        if (!wait_write()) {
            continue;
        }
        outb(0x60, command);
        result = read_response(true);
        drain_output();
        if (result == PS2_ACK_OK) {
            return result;
        }
    }
    return 0U;
}

static void push_key(uint16_t code, uint8_t pressed)
{
    uint32_t next = (key_write + 1U) % KEY_QUEUE_SIZE;

    if (next == key_read) {
        dropped_key_count++;
        return;
    }
    key_queue[key_write].code = code;
    key_queue[key_write].modifiers = modifiers;
    key_queue[key_write].pressed = pressed;
    key_write = next;
    key_event_count++;
}

static void push_mouse(int16_t delta_x, int16_t delta_y, int8_t wheel,
                       uint8_t buttons)
{
    uint32_t next = (mouse_write + 1U) % MOUSE_QUEUE_SIZE;

    if (next == mouse_read) {
        dropped_mouse_count++;
        return;
    }
    mouse_queue[mouse_write].delta_x = delta_x;
    mouse_queue[mouse_write].delta_y = delta_y;
    mouse_queue[mouse_write].wheel = wheel;
    mouse_queue[mouse_write].buttons = buttons;
    mouse_write = next;
}

static void apply_modifier(uint16_t code, bool active)
{
    uint16_t bit = 0;

    if (code == KEY_LEFT_SHIFT || code == KEY_RIGHT_SHIFT) {
        bit = MOD_SHIFT;
    } else if (code == KEY_LEFT_CTRL || code == KEY_RIGHT_CTRL) {
        bit = MOD_CTRL;
    } else if (code == KEY_LEFT_ALT || code == KEY_RIGHT_ALT) {
        bit = MOD_ALT;
    }
    if (bit == 0U) {
        return;
    }
    if (active) {
        modifiers |= bit;
    } else {
        modifiers &= (uint16_t)~bit;
    }
}

static uint16_t extended_map(uint8_t base)
{
    static const uint16_t table[] = {
        [0x1D] = KEY_RIGHT_CTRL,
        [0x14] = KEY_RIGHT_ALT,
        [0x38] = KEY_RIGHT_ALT,
        [0x47] = KEY_HOME,
        [0x48] = KEY_UP,
        [0x49] = KEY_PAGE_UP,
        [0x4B] = KEY_LEFT,
        [0x4D] = KEY_RIGHT,
        [0x4F] = KEY_END,
        [0x50] = KEY_DOWN,
        [0x51] = KEY_PAGE_DOWN,
        [0x52] = KEY_INSERT,
        [0x53] = KEY_DELETE
    };
    uint32_t index = base;

    if (index >= (uint32_t)(sizeof(table) / sizeof(table[0]))) {
        return KEY_NONE;
    }
    return table[index];
}

void input_handle_scancode(uint8_t scancode)
{
    bool releasing = release_pending;
    uint16_t code;

    key_scancode_count++;
    if (scancode == SCANCODE_EXTENDED) {
        extended_pending = true;
        return;
    }
    if (scancode == SCANCODE_RELEASE) {
        release_pending = true;
        return;
    }
    if ((scancode & 0x80U) != 0U) {
        uint8_t base = (uint8_t)(scancode & 0x7FU);
        uint16_t released = extended_pending ? extended_map(base)
                                             : scancode_map[base];

        extended_pending = false;
        release_pending = false;
        if (base < 128U && released != KEY_NONE) {
            apply_modifier(released, false);
            push_key(released, 0U);
        }
        return;
    }
    if (scancode >= 128U) {
        extended_pending = false;
        release_pending = false;
        return;
    }
    code = extended_pending ? extended_map(scancode) : scancode_map[scancode];
    extended_pending = false;
    release_pending = false;
    if (code == KEY_NONE) {
        return;
    }
    if (releasing) {
        apply_modifier(code, false);
        push_key(code, 0U);
        return;
    }
    apply_modifier(code, true);
    if (code == KEY_CAPS_LOCK) {
        modifiers ^= MOD_CAPS;
    }
    push_key(code, 1U);
}

void input_handle_mouse_packet(int16_t delta_x, int16_t delta_y,
                              uint8_t raw_buttons)
{
    input_handle_mouse_wheel(delta_x, delta_y, 0, raw_buttons);
}

void input_handle_mouse_wheel(int16_t delta_x, int16_t delta_y, int8_t wheel,
                              uint8_t raw_buttons)
{
    uint8_t buttons = 0;

    mouse_packet_count++;
    if (!mouse_liveness_reported) {
        mouse_liveness_reported = true;
        serial_write("[ps2 mouse] streaming on irq 12\n");
    }
    if ((raw_buttons & 0x04U) != 0U) {
        buttons |= MOUSE_BUTTON_MIDDLE;
    }
    if ((raw_buttons & 0x01U) != 0U) {
        buttons |= MOUSE_BUTTON_LEFT;
    }
    if ((raw_buttons & 0x02U) != 0U) {
        buttons |= MOUSE_BUTTON_RIGHT;
    }
    previous_buttons = buttons;
    push_mouse(delta_x, delta_y, wheel, buttons);
}

static void collect_mouse_byte(void)
{
    mouse_packet[mouse_packet_index++] = inb(0x60);
    mouse_irq_bytes++;
    if (mouse_packet_index < (ps2_mouse_wheel ? 4U : 3U)) {
        return;
    }
    mouse_packet_index = 0;
    {
        int16_t dx = (int16_t)mouse_packet[1];
        int16_t dy = (int16_t)mouse_packet[2];

        if ((mouse_packet[0] & 0x10U) != 0U) {
            dx = (int16_t)((int)dx - 256);
        }
        if ((mouse_packet[0] & 0x20U) != 0U) {
            dy = (int16_t)((int)dy - 256);
        }
        if (ps2_mouse_wheel) {
            int8_t wheel = (int8_t)(mouse_packet[3] & 0x0FU);

            /* The wheel is a signed nibble: 0xF is one notch away from the
             * user, not fifteen towards. */
            if ((wheel & 0x08U) != 0U) {
                wheel = (int8_t)((int)wheel - 16);
            }
            input_handle_mouse_wheel(dx, dy, wheel,
                                     (uint8_t)(mouse_packet[0] |
                                               (mouse_packet[3] & 0x30U)));
            mouse_packet_index = 0;
            return;
        }
        input_handle_mouse_packet(dx, dy, mouse_packet[0]);
        mouse_packet_index = 0;
    }
}

void input_keyboard_irq(void)
{
    uint32_t guard = PS2_IRQ_DRAIN_LIMIT;

    keyboard_irq_calls++;
    while (guard-- != 0U) {
        uint8_t status = inb(0x64);

        keyboard_last_status = status;
        if ((status & PS2_STATUS_OUTPUT_FULL) == 0U) {
            keyboard_irq_empty++;
            break;
        }
        if ((status & PS2_STATUS_MOUSE) != 0U) {
            keyboard_irq_aux++;
            collect_mouse_byte();
            continue;
        }
        input_handle_scancode(inb(0x60));
        keyboard_irq_bytes++;
    }
}

void input_mouse_irq(void)
{
    uint32_t guard = PS2_IRQ_DRAIN_LIMIT;

    mouse_irq_calls++;
    while (guard-- != 0U) {
        uint8_t status = inb(0x64);

        mouse_last_status = status;
        if ((status & PS2_STATUS_OUTPUT_FULL) == 0U) {
            mouse_irq_no_data++;
            break;
        }
        if ((status & PS2_STATUS_MOUSE) == 0U) {
            mouse_irq_not_mouse++;
            input_handle_scancode(inb(0x60));
            keyboard_irq_bytes++;
            continue;
        }
        collect_mouse_byte();
    }
}

void input_init(void)
{
    uint8_t config;
    uint8_t ack;

    drain_output();
    (void)send_command(0xA8);
    config = send_command(0x20);
    if (config == 0x00U || config == 0xFFU) {
        config = 0x47U;
    } else {
        config |= 0x07U;
    }
    write_command_byte(config);
    (void)send_command(0xAA);
    (void)send_command(0xAE);
    ps2_config_byte = send_command(0x20);

    drain_output();
    (void)send_command(0xA8);
    /* Default parameters, then the wheel, and only then enable reporting.
     *
     * The order matters and getting it wrong is why the wheel never worked.
     * 0xF4 starts the device sending packets, and every packet that follows is
     * on the same port as the acknowledgements the commands return.  Ask for
     * the wheel after that and the response is read out of the middle of the
     * packet stream, so the command reports no answer -- which is what "ack 0"
     * was.  Negotiated while the device is still quiet, the answer is the only
     * thing on the port and is read correctly. */
    ack = send_aux_command(0xF6);
    ps2_ack_f6 = ack;
    ack = send_aux_command(0xF4);
    ps2_ack_f4 = ack;
    (void)send_aux_command(0xF3);
    (void)send_aux_command(100);

    /* The magic sample sequence: 200, 80, 200, then read back a device id.  A
     * mouse with a wheel answers 3; one without does not answer with that. */
    (void)send_aux_command(0xF3);
    (void)send_aux_command(200);
    (void)send_aux_command(0xF3);
    (void)send_aux_command(80);
    (void)send_aux_command(0xF3);
    (void)send_aux_command(200);
    {
        uint8_t wheel_ack = send_aux_command(0xF3);
        uint8_t wheel_id = read_response(true);

        ps2_mouse_wheel = wheel_ack == PS2_ACK_OK && wheel_id == 3U;
        serial_write("[ps2 mouse] wheel ack ");
        serial_write_decimal(wheel_ack);
        serial_write(" id ");
        serial_write_decimal(wheel_id);
        serial_putc('\n');
    }
    /* reporting on, last */
    (void)send_aux_command(0xF4);

    ps2_status_probe = inb(0x64);
    mouse_packet_index = 0;
    mouse_packet[0] = 0;
    mouse_packet[1] = 0;
    mouse_packet[2] = 0;
    previous_buttons = 0;
    extended_pending = false;
    release_pending = false;
    key_read = 0;
    key_write = 0;
    mouse_read = 0;
    mouse_write = 0;
}

bool input_poll_key(struct key_event *event)
{
    if (key_read == key_write) {
        return false;
    }
    *event = key_queue[key_read];
    key_read = (key_read + 1U) % KEY_QUEUE_SIZE;
    return true;
}

bool input_poll_mouse(struct mouse_event *event)
{
    if (mouse_read == mouse_write) {
        return false;
    }
    *event = mouse_queue[mouse_read];
    mouse_read = (mouse_read + 1U) % MOUSE_QUEUE_SIZE;
    return true;
}

bool input_key_is_named(uint16_t code)
{
    return code >= KEY_ESCAPE;
}

bool input_key_is_modifier(uint16_t code)
{
    return code >= KEY_LEFT_SHIFT && code <= KEY_CAPS_LOCK;
}

char input_key_ascii(const struct key_event *event)
{
    uint16_t code = event->code;

    if (event->pressed == 0U) {
        return 0;
    }
    if (code == KEY_ENTER) {
        return '\n';
    }
    if (code == KEY_TAB) {
        return '\t';
    }
    if (code == KEY_BACKSPACE) {
        return '\b';
    }
    if (code == KEY_SPACE) {
        return ' ';
    }
    if (code < KEY_CHAR_BASE || code > KEY_CHAR_LAST) {
        return 0;
    }
    if ((event->modifiers & MOD_ALT) != 0U) {
        if (code >= 'a' && code <= 'z') {
            return (char)(code - 'a' + 1);
        }
        return 0;
    }
    if ((event->modifiers & MOD_SHIFT) != 0U) {
        return shifted_map[code];
    }
    if (code >= 'a' && code <= 'z' && (event->modifiers & MOD_CAPS) != 0U) {
        return (char)(code - 'a' + 'A');
    }
    return (char)code;
}

const char *input_key_name(uint16_t code)
{
    uint32_t index;

    if (code < KEY_NAMED_BASE) {
        return "none";
    }
    index = (uint32_t)code - KEY_NAMED_BASE;
    if (index >= (uint32_t)(KEY_CODE_COUNT - KEY_NAMED_BASE)) {
        return "none";
    }
    return key_names[index];
}

uint8_t input_mouse_config(void)
{
    return ps2_config_byte;
}

bool input_mouse_ready(void)
{
    return ps2_mouse_configured;
}

uint32_t input_mouse_packets(void)
{
    return mouse_packet_count;
}

uint32_t input_key_scancodes(void)
{
    return key_scancode_count;
}

uint32_t input_key_events(void)
{
    return key_event_count;
}

uint32_t input_dropped_keys(void)
{
    return dropped_key_count;
}

uint32_t input_dropped_mouse(void)
{
    return dropped_mouse_count;
}
