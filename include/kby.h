#ifndef KLYE_KBY_H
#define KLYE_KBY_H

#include <stdbool.h>
#include <stdint.h>

#include "gfx.h"

#define KBY_MAGIC 0x3147424BU
#define KBY_VERSION 1U
#define KBY_HEADER_SIZE 8U
#define KBY_MAX_CODE 4096U
#define KBY_STACK_SLOTS 64
#define KBY_VARS 32
#define KBY_KEYS 16
#define KBY_WIN_MAX 4
#define KBY_MAX_APPS 6
/* Per-frame instruction cap.  This used to be 2,000,000, which was high
 * enough that no program we tested could reach it, so it never actually
 * bounded anything.  A frame that runs out of budget resumes at the same
 * instruction next frame, so this needs to be comfortably above a normal
 * frame while still stopping a runaway loop from wedging the compositor. */
#define KBY_BUDGET 50000U
#define KBY_NAME_MAX 32

enum kby_opcode {
    KBY_OP_NOP = 0x00,
    KBY_OP_PUSH8 = 0x01,
    KBY_OP_PUSH32 = 0x02,
    KBY_OP_PUSHSTR = 0x03,
    KBY_OP_POP = 0x04,
    KBY_OP_DUP = 0x05,
    KBY_OP_LOAD = 0x06,
    KBY_OP_STORE = 0x07,
    KBY_OP_ADD = 0x08,
    KBY_OP_SUB = 0x09,
    KBY_OP_MUL = 0x0A,
    KBY_OP_DIV = 0x0B,
    KBY_OP_MOD = 0x0C,
    KBY_OP_CMP = 0x65,
    KBY_OP_JMP = 0x0D,
    KBY_OP_JZ = 0x0E,
    KBY_OP_JNZ = 0x0F,
    KBY_OP_CALL = 0x10,
    KBY_OP_RET = 0x11,
    KBY_OP_HALT = 0x12,

    KBY_OP_PRINT = 0x20,
    KBY_OP_PRINTLN = 0x21,

    KBY_OP_DRAW_CLEAR = 0x30,
    KBY_OP_DRAW_RECT = 0x31,
    KBY_OP_DRAW_ROUNDED = 0x32,
    KBY_OP_DRAW_BORDER = 0x33,
    KBY_OP_DRAW_PIXEL = 0x34,
    KBY_OP_DRAW_CIRCLE = 0x35,
    KBY_OP_DRAW_LINE = 0x36,
    KBY_OP_DRAW_TEXT = 0x37,
    KBY_OP_DRAW_TEXT_CENTER = 0x38,
    KBY_OP_FILL_HEX = 0x39,

    KBY_OP_VFS_EXISTS = 0x50,
    KBY_OP_VFS_SIZE = 0x51,
    KBY_OP_VFS_READ = 0x52,
    KBY_OP_VFS_WRITE = 0x53,
    KBY_OP_VFS_APPEND = 0x54,

    KBY_OP_KEY_POLL = 0x61,
    KBY_OP_MOUSE_X = 0x62,
    KBY_OP_MOUSE_Y = 0x63,
    KBY_OP_MOUSE_DOWN = 0x64,
    KBY_OP_FRAME = 0x67,
    KBY_OP_TICKS = 0x60,
    KBY_OP_NUM = 0x66,
    KBY_OP_WIN_OPEN = 0x68,
    KBY_OP_WIN_CLOSE = 0x69,
    KBY_OP_VSYNC = 0x6A
};

struct kby_app;

bool kby_valid(const uint8_t *code, uint32_t length);
struct kby_app *kby_load(const char *name, const uint8_t *code, uint32_t length);
void kby_unload(struct kby_app *app);
int kby_app_count(void);
struct kby_app *kby_app_at(int index);
struct kby_app *kby_find(const char *name);
int kby_app_slot(struct kby_app *app);

/* input delivery from the window manager */
void kby_push_key(struct kby_app *app, uint32_t code, bool pressed);
void kby_set_mouse(struct kby_app *app, int x, int y, bool down);

/* window control from inside a script */
bool kby_open_window(const char *title, int width, int height);
void kby_close_self(void);

bool kby_run(struct kby_app *app, uint32_t budget);
void kby_flush_output(struct kby_app *app);
void kby_draw(struct kby_app *app, struct gfx_surface *surface, int x, int y,
              int width, int height);
void kby_clear_display(struct kby_app *app);

const char *kby_app_title(struct kby_app *app);
const char *kby_error(struct kby_app *app);
const char *kby_last_error(void);
uint32_t kby_app_steps(struct kby_app *app);
bool kby_app_starved(struct kby_app *app);
int32_t kby_app_result(struct kby_app *app);
bool kby_app_loaded(struct kby_app *app);

#endif
