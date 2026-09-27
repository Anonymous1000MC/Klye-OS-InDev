#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "apps.h"
#include "font.h"
#include "gfx.h"
#include "kby.h"
#include "theme.h"
#include "vfs.h"

#define KBY_LIST_MAX 64
#define KBY_TEXT_MAX 4096
#define KBY_ARENA_BYTES 4608
#define KBY_MAX_FRAMES 64
#define KBY_FRAMES KBY_MAX_FRAMES
#define KBY_ARENA_SLOT 24
#define KBY_CMD_TEXT 48
#define KBY_OUT_MAX 512

struct kby_cmd {
    uint8_t op;
    int32_t a;
    int32_t b;
    int32_t c;
    int32_t d;
    char text[KBY_CMD_TEXT];
};

struct kby_app {
    char scratch[KBY_CMD_TEXT];
    int scratch_len;
    bool used;
    bool dead;
    bool halted;
    char name[KBY_NAME_MAX];
    char title[KBY_NAME_MAX];
    char error[64];
    uint8_t code[KBY_MAX_CODE];
    uint32_t code_length;
    uint32_t pc;
    uint32_t call_depth;
    uint32_t returns[KBY_MAX_FRAMES];
    int32_t stack[KBY_STACK_SLOTS];
    int sp;
    char arena[KBY_ARENA_BYTES];
    int arena_used;
    int32_t vars[KBY_VARS * KBY_MAX_FRAMES];
    int32_t *locals;
    struct kby_cmd list[KBY_LIST_MAX];
    int list_count;
    int32_t result;
    int32_t radius;
    bool has_result;
    uint32_t steps;
    uint32_t slices;
};

static struct kby_app apps[KBY_MAX_APPS];
static char last_error[64];
static char kby_out[KBY_OUT_MAX];
static int kby_out_len;

static bool same(const char *a, const char *b)
{
    int index = 0;

    while (a[index] != 0 && a[index] == b[index]) {
        ++index;
    }
    return a[index] == b[index];
}

static void take(char *destination, int max, const char *source)
{
    int index = 0;

    if (source == 0) {
        destination[0] = 0;
        return;
    }
    while (source[index] != 0 && index < max - 1) {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = 0;
}

static int length_of(const char *text)
{
    int count = 0;

    if (text == 0) {
        return 0;
    }
    while (text[count] != 0) {
        ++count;
    }
    return count;
}

static void fail(struct kby_app *app, const char *why)
{
    app->dead = true;
    take(app->error, (int)sizeof(app->error), why);
    take(last_error, (int)sizeof(last_error), why);
}

const char *kby_last_error(void)
{
    return last_error[0] != 0 ? last_error : 0;
}

static void emit(struct kby_app *app, uint8_t op, int32_t a, int32_t b,
                 int32_t c, int32_t d, const char *text)
{
    struct kby_cmd *cmd;
    int index = 0;

    if (app->list_count >= KBY_LIST_MAX) {
        fail(app, "display list full");
        return;
    }
    cmd = &app->list[app->list_count++];
    cmd->op = op;
    cmd->a = a;
    cmd->b = b;
    cmd->c = c;
    cmd->d = d;
    if (text != 0) {
        while (text[index] != 0 && index < KBY_CMD_TEXT - 1) {
            cmd->text[index] = text[index];
            ++index;
        }
    }
    cmd->text[index] = 0;
}

static void out_append(struct kby_app *app, const char *text, bool newline)
{
    int index = 0;

    (void)app;
    if (text == 0) {
        return;
    }
    if (kby_out_len < 0 || kby_out_len >= KBY_OUT_MAX - 2) {
        kby_out_len = 0;
    }
    while (text[index] != 0 && kby_out_len < KBY_OUT_MAX - 2) {
        kby_out[kby_out_len++] = text[index];
        ++index;
    }
    if (newline != false && kby_out_len < KBY_OUT_MAX - 1) {
        kby_out[kby_out_len++] = '\n';
    }
    kby_out[kby_out_len] = 0;
}

static void push(struct kby_app *app, int32_t value)
{
    if (app->sp >= KBY_STACK_SLOTS) {
        fail(app, "stack overflow");
        return;
    }
    app->stack[app->sp++] = value;
}

static bool pop(struct kby_app *app, int32_t *out)
{
    if (app->sp <= 0) {
        fail(app, "stack underflow");
        return false;
    }
    --app->sp;
    *out = app->stack[app->sp];
    return true;
}

static bool fetch8(struct kby_app *app, uint32_t *out)
{
    if (app->pc >= app->code_length) {
        fail(app, "pc ran off the end of the program");
        return false;
    }
    *out = app->code[app->pc++];
    return true;
}

static bool fetch16(struct kby_app *app, uint32_t *out)
{
    uint32_t low;
    uint32_t high;

    if (!fetch8(app, &low) || !fetch8(app, &high)) {
        return false;
    }
    *out = low | (high << 8);
    return true;
}

static bool fetch32(struct kby_app *app, uint32_t *out)
{
    uint32_t value = 0;

    for (int index = 0; index < 4; ++index) {
        uint32_t byte;

        if (!fetch8(app, &byte)) {
            return false;
        }
        value |= byte << (index * 8);
    }
    *out = value;
    return true;
}

/* Strings live in the app's own code buffer, so the pointer stays valid for
 * the lifetime of the app and PUSHSTR needs no separate string table. */
static const char *fetch_str(struct kby_app *app, uint32_t *out_length)
{
    uint32_t length;
    const char *text;

    if (!fetch16(app, &length)) {
        return 0;
    }
    if (length > app->code_length - app->pc) {
        fail(app, "string runs past the end of the program");
        return 0;
    }
    text = (const char *)(const void *)&app->code[app->pc];
    app->pc += length;
    if (out_length != 0) {
        *out_length = length;
    }
    return text;
}

bool kby_valid(const uint8_t *code, uint32_t length)
{
    if (code == 0 || length < 12U) {
        return false;
    }
    if (code[0] != 'K' || code[1] != 'B' || code[2] != 'Y' ||
        code[3] != '1') {
        return false;
    }
    return code[4] == KBY_VERSION;
}

struct kby_app *kby_load(const char *name, const uint8_t *code, uint32_t length)
{
    struct kby_app *slot = 0;

    if (!kby_valid(code, length)) {
        take(last_error, (int)sizeof(last_error), "not a KBY1 image");
        return 0;
    }
    if (length > KBY_MAX_CODE) {
        take(last_error, (int)sizeof(last_error), "KBY image too large");
        return 0;
    }
    for (int index = 0; index < KBY_MAX_APPS; ++index) {
        if (!apps[index].used) {
            slot = &apps[index];
            break;
        }
    }
    if (slot == 0) {
        take(last_error, (int)sizeof(last_error), "no free app slot");
        return 0;
    }
    take(slot->name, (int)sizeof(slot->name), name);
    take(slot->title, (int)sizeof(slot->title), name);
    slot->error[0] = 0;
    /* Keep only the body so that pc, jump targets and kbasm's label offsets
     * all share one coordinate system starting at zero. */
    __builtin_memcpy(slot->code, code + KBY_HEADER_SIZE,
                     (size_t)(length - KBY_HEADER_SIZE));
    slot->code_length = length - KBY_HEADER_SIZE;
    slot->pc = 0U;
    slot->locals = slot->vars;
    slot->call_depth = 0U;
    slot->sp = 0;
    slot->arena_used = 0;
    kby_out_len = 0;
    kby_out[0] = 0;
    slot->list_count = 0;
    slot->result = 0;
    slot->has_result = false;
    slot->steps = 0U;
    slot->slices = 0U;
    slot->dead = false;
    slot->used = true;
    for (int index = 0; index < KBY_STACK_SLOTS; ++index) {
        slot->stack[index] = 0;
    }
    for (int index = 0; index < KBY_VARS * KBY_MAX_FRAMES; ++index) {
        slot->vars[index] = 0;
    }
    last_error[0] = 0;
    return slot;
}

void kby_unload(struct kby_app *app)
{
    if (app == 0) {
        return;
    }
    app->used = false;
    app->dead = false;
    app->halted = false;
    app->sp = 0;
    app->list_count = 0;
    app->call_depth = 0U;
    kby_out_len = 0;
    kby_out[0] = 0;
    app->locals = app->vars;
    app->code_length = 0U;
}

int kby_app_count(void)
{
    int total = 0;

    for (int index = 0; index < KBY_MAX_APPS; ++index) {
        if (apps[index].used) {
            ++total;
        }
    }
    return total;
}

struct kby_app *kby_app_at(int index)
{
    if (index < 0 || index >= KBY_MAX_APPS || !apps[index].used) {
        return 0;
    }
    return &apps[index];
}

struct kby_app *kby_find(const char *name)
{
    for (int index = 0; index < KBY_MAX_APPS; ++index) {
        if (apps[index].used && same(apps[index].name, name)) {
            return &apps[index];
        }
    }
    return 0;
}

const char *kby_app_title(struct kby_app *app)
{
    if (app == 0) {
        return "";
    }
    return app->title[0] != 0 ? app->title : app->name;
}

bool kby_app_loaded(struct kby_app *app)
{
    return app != 0 && app->used && !app->dead && !app->halted;
}

const char *kby_error(struct kby_app *app)
{
    if (app == 0 || app->error[0] == 0) {
        return 0;
    }
    return app->error;
}

uint32_t kby_app_steps(struct kby_app *app)
{
    return app != 0 ? app->steps : 0U;
}

int32_t kby_app_result(struct kby_app *app)
{
    return app != 0 ? app->result : 0;
}

bool kby_run(struct kby_app *app, uint32_t budget)
{
    uint32_t spent = 0;

    if (!kby_app_loaded(app)) {
        return false;
    }
    while (spent < budget) {
        uint32_t opcode;

        if (!fetch8(app, &opcode)) {
            return false;
        }
        ++spent;
        ++app->steps;

        switch (opcode) {
        case KBY_OP_NOP:
            break;
        case KBY_OP_PUSH8: {
            uint32_t value;

            if (!fetch8(app, &value)) {
                return false;
            }
            push(app, (int32_t)value);
            break;
        }
        case KBY_OP_PUSH32: {
            uint32_t value;

            if (!fetch32(app, &value)) {
                return false;
            }
            push(app, (int32_t)value);
            break;
        }
        case KBY_OP_PUSHSTR: {
            uint32_t length = 0;
            const char *text = fetch_str(app, &length);
            char *slot;

            if (text == 0) {
                return false;
            }
            if (app->arena_used + length + 1U > KBY_ARENA_BYTES) {
                fail(app, "string arena exhausted");
                return false;
            }
            slot = &app->arena[app->arena_used];
            __builtin_memcpy(slot, text, (size_t)length);
            slot[length] = 0;
            app->arena_used += length + 1U;
            push(app, (int32_t)(uintptr_t)slot);
            break;
        }
        case KBY_OP_POP: {
            int32_t discard;

            if (!pop(app, &discard)) {
                return false;
            }
            break;
        }
        case KBY_OP_DUP: {
            int32_t value;

            if (app->sp <= 0) {
                fail(app, "stack underflow");
                return false;
            }
            value = app->stack[app->sp - 1];
            push(app, value);
            break;
        }
        case KBY_OP_LOAD: {
            uint32_t slot;

            if (!fetch8(app, &slot)) {
                return false;
            }
            if (slot >= KBY_VARS) {
                fail(app, "load: slot out of range");
                return false;
            }
            push(app, app->locals[slot]);
            break;
        }
        case KBY_OP_STORE: {
            uint32_t slot;
            int32_t value;

            if (!fetch8(app, &slot)) {
                return false;
            }
            if (slot >= KBY_VARS) {
                fail(app, "store: slot out of range");
                return false;
            }
            if (!pop(app, &value)) {
                return false;
            }
            app->locals[slot] = value;
            break;
        }
        case KBY_OP_CMP: {
            int32_t top;
            int32_t below;

            if (!pop(app, &top) || !pop(app, &below)) {
                return false;
            }
            push(app, top < below ? 1 : 0);
            break;
        }
        case KBY_OP_ADD:
        case KBY_OP_SUB:
        case KBY_OP_MUL:
        case KBY_OP_DIV:
        case KBY_OP_MOD: {
            int32_t left;
            int32_t right;

            if (!pop(app, &left) || !pop(app, &right)) {
                return false;
            }
            if ((opcode == KBY_OP_DIV || opcode == KBY_OP_MOD) && right == 0) {
                fail(app, "divide by zero");
                return false;
            }
            if (opcode == KBY_OP_ADD) {
                push(app, (int32_t)((uint32_t)left + (uint32_t)right));
            } else if (opcode == KBY_OP_SUB) {
                push(app, (int32_t)((uint32_t)right - (uint32_t)left));
            } else if (opcode == KBY_OP_MUL) {
                push(app, (int32_t)((uint32_t)left * (uint32_t)right));
            } else if (opcode == KBY_OP_DIV) {
                push(app, right / left);
            } else {
                push(app, right % left);
            }
            break;
        }
        case KBY_OP_JMP: {
            uint32_t target;

            if (!fetch16(app, &target)) {
                return false;
            }
            if (target >= app->code_length) {
                fail(app, "jump target out of range");
                return false;
            }
            app->pc = target;
            break;
        }
        case KBY_OP_JZ:
        case KBY_OP_JNZ: {
            uint32_t target;
            int32_t value;

            if (!fetch16(app, &target)) {
                return false;
            }
            if (target >= app->code_length) {
                fail(app, "jump target out of range");
                return false;
            }
            if (!pop(app, &value)) {
                return false;
            }
            if ((opcode == KBY_OP_JZ && value == 0) ||
                (opcode == KBY_OP_JNZ && value != 0)) {
                app->pc = target;
            }
            break;
        }
        case KBY_OP_CALL: {
            uint32_t target;

            if (!fetch16(app, &target)) {
                return false;
            }
            if (target >= app->code_length) {
                fail(app, "call target out of range");
                return false;
            }
            if (app->call_depth >= 64U) {
                fail(app, "call depth exceeded");
                return false;
            }
            if (app->sp >= KBY_FRAMES + KBY_STACK_SLOTS) {
                fail(app, "no room for a call frame");
                return false;
            }
            {
                uint32_t next = app->call_depth + 1U;
                uint32_t base = next * (uint32_t)KBY_VARS;

                app->returns[app->call_depth] = app->pc;
                for (uint32_t index = 0; index < KBY_VARS; ++index) {
                    app->vars[base + index] = 0;
                }
                ++app->call_depth;
                app->locals = &app->vars[base];
            }
            app->pc = target;
            break;
        }
        case KBY_OP_RET: {
            if (app->call_depth == 0U) {
                if (app->sp <= 0) {
                    fail(app, "return without a value");
                    return false;
                }
                return true;
            }
            --app->call_depth;
            app->locals = &app->vars[app->call_depth * KBY_VARS];
            app->pc = app->returns[app->call_depth];
            if (app->pc >= app->code_length) {
                fail(app, "return address out of range");
                return false;
            }
            break;
        }
        case KBY_OP_HALT:
            if (app->sp > 0) {
                --app->sp;
                app->result = app->stack[app->sp];
                app->has_result = true;
            }
            app->halted = true;
            return false;

        case KBY_OP_PRINT:
        case KBY_OP_PRINTLN: {
            const char *text = (const char *)(uintptr_t)0;
            int32_t pointer;

            if (!pop(app, &pointer)) {
                return false;
            }
            text = (const char *)(uintptr_t)pointer;
            if (text == 0) {
                fail(app, "print: expected a string");
                return false;
            }
            out_append(app, text, opcode == KBY_OP_PRINTLN);
            break;
        }

        case KBY_OP_DRAW_CLEAR:
            emit(app, opcode, 0, 0, 0, 0, 0);
            break;
        case KBY_OP_DRAW_RECT:
        case KBY_OP_DRAW_ROUNDED:
        case KBY_OP_DRAW_BORDER: {
            int32_t colour;
            int32_t height;
            int32_t width;
            int32_t top;
            int32_t left;
            int32_t radius = 0;

            if (opcode != KBY_OP_DRAW_RECT) {
                uint32_t raw;

                if (!fetch8(app, &raw)) {
                    return false;
                }
                radius = (int32_t)raw;
            }
            if (!pop(app, &colour) || !pop(app, &height) ||
                !pop(app, &width) || !pop(app, &top) || !pop(app, &left)) {
                return false;
            }
            app->result = colour;
            app->radius = radius;
            emit(app, (uint8_t)opcode, left, top, width, height, 0);
            app->radius = 0;
            emit(app, KBY_OP_FILL_HEX, 0, 0, 0, 0, 0);
            break;
        }
        case KBY_OP_DRAW_PIXEL:
        case KBY_OP_DRAW_CIRCLE:
        case KBY_OP_DRAW_LINE: {
            int32_t colour;
            int32_t third;
            int32_t second;
            int32_t first;

            if (!pop(app, &colour) || !pop(app, &third) || !pop(app, &second) ||
                !pop(app, &first)) {
                return false;
            }
            emit(app, (uint8_t)opcode, first, second, third, colour, 0);
            break;
        }
        case KBY_OP_DRAW_TEXT:
        case KBY_OP_DRAW_TEXT_CENTER: {
            const char *text;
            int32_t colour;
            int32_t top;
            int32_t left;

            text = fetch_str(app, 0);
            if (text == 0) {
                return false;
            }
            if (!pop(app, &colour) || !pop(app, &top) || !pop(app, &left)) {
                return false;
            }
            emit(app, (uint8_t)opcode, left, top, colour, 0, text);
            break;
        }

        case KBY_OP_VFS_EXISTS:
        case KBY_OP_VFS_SIZE: {
            int32_t pointer;

            if (!pop(app, &pointer)) {
                return false;
            }
            if (pointer == 0) {
                fail(app, "vfs: expected a path on the stack");
                return false;
            }
            if (opcode == KBY_OP_VFS_EXISTS) {
                push(app, vfs_exists((const char *)(uintptr_t)pointer) ? 1
                                                                       : 0);
            } else {
                int node = vfs_resolve((const char *)(uintptr_t)pointer, 0);

                push(app, node > 0 ? (int32_t)vfs_size(node) : -1);
            }
            break;
        }
        case KBY_OP_VFS_READ: {
            char path[VFS_PATH_MAX];
            int32_t pointer;
            const char *text;
            char *body;
            int length;

            if (!pop(app, &pointer)) {
                return false;
            }
            text = (const char *)(uintptr_t)pointer;
            if (text == 0) {
                fail(app, "read: missing path");
                return false;
            }
            take(path, (int)sizeof(path), text);
            if (app->arena_used + KBY_TEXT_MAX > KBY_ARENA_BYTES) {
                app->arena_used = 0;
            }
            body = &app->arena[app->arena_used];
            app->arena_used += KBY_TEXT_MAX;
            length = vfs_read(path, body, (uint32_t)KBY_TEXT_MAX - 1U);
            if (length < 0) {
                push(app, -1);
            } else {
                body[length] = 0;
                push(app, (int32_t)(uintptr_t)body);
            }
            break;
        }
        case KBY_OP_VFS_WRITE: {
            char path[VFS_PATH_MAX];
            int32_t data_pointer;
            int32_t path_pointer;
            const char *path_text;
            const char *data_text;
            int length;

            if (!pop(app, &data_pointer) || !pop(app, &path_pointer)) {
                return false;
            }
            path_text = (const char *)(uintptr_t)path_pointer;
            data_text = (const char *)(uintptr_t)data_pointer;
            if (path_text == 0 || data_text == 0) {
                fail(app, "write: missing path or data");
                return false;
            }
            take(path, (int)sizeof(path), path_text);
            length = vfs_write(path, data_text, (uint32_t)length_of(data_text));
            push(app, length < 0 ? -1 : length);
            break;
        }
        case KBY_OP_VFS_APPEND: {
            char path[VFS_PATH_MAX];
            int32_t data_pointer;
            int32_t path_pointer;
            const char *path_text;
            const char *data_text;
            int length;

            if (!pop(app, &data_pointer) || !pop(app, &path_pointer)) {
                return false;
            }
            path_text = (const char *)(uintptr_t)path_pointer;
            data_text = (const char *)(uintptr_t)data_pointer;
            if (path_text == 0 || data_text == 0) {
                fail(app, "append: missing path or data");
                return false;
            }
            take(path, (int)sizeof(path), path_text);
            length = vfs_append(path, data_text, (uint32_t)length_of(data_text));
            push(app, length < 0 ? -1 : length);
            break;
        }

        case KBY_OP_NUM: {
            int32_t value;
            char digits[KBY_ARENA_SLOT];
            int at = 0;
            int start;
            unsigned int magnitude;

            if (!pop(app, &value)) {
                return false;
            }
            if (value < 0) {
                digits[at++] = '-';
                magnitude = (unsigned int)(-(int64_t)value);
            } else {
                magnitude = (unsigned int)value;
            }
            start = at;
            if (magnitude == 0U) {
                digits[at++] = '0';
            }
            while (magnitude != 0U && at < KBY_ARENA_SLOT - 1) {
                digits[at++] = (char)('0' + (int)(magnitude % 10U));
                magnitude /= 10U;
            }
            digits[at] = 0;
            for (int left = start, right = at - 1; left < right;
                 ++left, --right) {
                char swap = digits[left];

                digits[left] = digits[right];
                digits[right] = swap;
            }
            if (at >= KBY_CMD_TEXT) {
                at = KBY_CMD_TEXT - 1;
            }
            digits[at] = 0;
            if (app->arena_used + KBY_ARENA_SLOT > KBY_ARENA_BYTES) {
                app->arena_used = 0;
            }
            __builtin_memcpy(&app->arena[app->arena_used], digits,
                             (size_t)at + 1U);
            push(app, (int32_t)(uintptr_t)&app->arena[app->arena_used]);
            app->arena_used += KBY_ARENA_SLOT;
            break;
        }
        case KBY_OP_TICKS: {
            extern uint64_t pit_ticks(void);

            push(app, (int32_t)pit_ticks());
            break;
        }
        default: {
            char detail[48];
            extern void serial_write(const char *);
            char trace[24];
            static const char hexd[] = "0123456789ABCDEF";

            trace[0] = 's';
            trace[1] = 'p';
            trace[2] = '=';
            trace[3] = hexd[(app->sp >> 4) & 0xF];
            trace[4] = hexd[app->sp & 0xF];
            trace[5] = ' ';
            trace[6] = 'd';
            trace[7] = 'p';
            trace[8] = '=';
            trace[9] = hexd[(app->call_depth >> 4) & 0xF];
            trace[10] = hexd[app->call_depth & 0xF];
            trace[11] = 0;
            serial_write("[kby ");
            serial_write(trace);
            serial_write("]\n");
            int at = 0;
            static const char digits[] = "0123456789ABCDEF";

            detail[0] = 'b';
            detail[1] = 'a';
            detail[2] = 'd';
            detail[3] = ' ';
            detail[4] = 'o';
            detail[5] = 'p';
            detail[6] = 'c';
            detail[7] = 'o';
            detail[8] = 'd';
            detail[9] = 'e';
            detail[10] = ' ';
            detail[11] = '0';
            detail[12] = 'x';
            detail[13] = digits[(opcode >> 4) & 0xF];
            detail[14] = digits[opcode & 0xF];
            detail[15] = ' ';
            detail[16] = 'a';
            detail[17] = 't';
            detail[18] = ' ';
            detail[19] = 'p';
            detail[20] = 'c';
            detail[21] = ' ';
            detail[22] = '0';
            detail[23] = 'x';
            at = 24;
            for (int shift = 20; shift >= 0; shift -= 4) {
                detail[at++] = digits[(app->pc >> shift) & 0xF];
            }
            detail[at] = 0;
            fail(app, detail);
            return false;
        }
        }
        if (app->dead) {
            return false;
        }
    }
    return true;
}

void kby_flush_output(struct kby_app *app)
{
    if (app == 0) {
        return;
    }
    if (kby_out_len > 0) {
        terminal_puts(kby_out);
        kby_out_len = 0;
        kby_out[0] = 0;
    }
    app->list_count = 0;
    ++app->slices;
}

void kby_draw(struct kby_app *app, struct gfx_surface *surface, int x, int y,
              int width, int height)
{
    uint32_t colour = THEME_SURFACE;
    int32_t radius = 0;

    if (app == 0) {
        return;
    }
    gfx_fill(surface, x, y, width, height, THEME_SURFACE);
    for (int index = 0; index < app->list_count; ++index) {
        struct kby_cmd *cmd = &app->list[index];
        int left = x + cmd->a;
        int top = y + cmd->b;

        switch (cmd->op) {
        case KBY_OP_FILL_HEX:
            colour = (uint32_t)app->result;
            radius = app->radius;
            break;
        case KBY_OP_DRAW_RECT:
            gfx_fill(surface, left, top, cmd->c, cmd->d, colour);
            break;
        case KBY_OP_DRAW_ROUNDED:
            gfx_rounded_rect(surface, left, top, cmd->c, cmd->d,
                             radius > 0 ? radius : 6, colour);
            break;
        case KBY_OP_DRAW_BORDER:
            gfx_rounded_border(surface, left, top, cmd->c, cmd->d,
                               radius > 0 ? radius : 6, 1, colour);
            break;
        case KBY_OP_DRAW_PIXEL:
            gfx_pixel(surface, left, top, colour);
            break;
        case KBY_OP_DRAW_CIRCLE:
            gfx_circle(surface, left, top, cmd->c, colour);
            break;
        case KBY_OP_DRAW_LINE:
            gfx_horizontal_line(surface, left, top, cmd->c, colour);
            break;
        case KBY_OP_DRAW_TEXT:
            font_draw(surface, left, top, cmd->text, colour, 1);
            break;
        case KBY_OP_DRAW_TEXT_CENTER:
            font_draw_centered(surface, left, top, cmd->text, colour, 1);
            break;
        default:
            break;
        }
    }
}

void kby_clear_display(struct kby_app *app)
{
    if (app != 0) {
        app->list_count = 0;
    }
}
