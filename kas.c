/* kas.c - the .kby assembler, compiled into the kernel.
 *
 * This is a freestanding reimplementation of tools/kbasm: no libc, no stdio.
 * It shares include/kby_ops.h with the host tool so the two can never accept
 * different mnemonics.
 *
 * Output must be byte-identical to the host assembler for the same input.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kby_ops.h"
#include "kas.h"

#define KAS_LABELS 128
#define KAS_LABEL_NAME 40
#define KAS_FIXUPS 192
#define KAS_LINE 256
#define KAS_HEADER_SIZE 8

struct label {
    char name[KAS_LABEL_NAME];
    uint32_t target;
    bool used;
};

struct fixup {
    int where;              /* byte offset of the 16-bit field to patch */
    char name[KAS_LABEL_NAME];
    bool used;
};

static struct label labels[KAS_LABELS];
static struct fixup fixups[KAS_FIXUPS];
static int label_count;
static int fixup_count;
static uint8_t *out;
static int out_length;
static int out_max;
static const char *kas_error;

static void kas_fail(const char *why)
{
    if (kas_error == 0) {
        kas_error = why;
    }
}

static bool kas_streq(const char *a, const char *b)
{
    while (*a != 0 && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

static bool kas_starts(const char *text, const char *prefix)
{
    while (*prefix != 0) {
        if (*text != *prefix) {
            return false;
        }
        ++text;
        ++prefix;
    }
    return true;
}

static int kas_strlen(const char *text)
{
    int count = 0;

    while (text[count] != 0) {
        ++count;
    }
    return count;
}

static void kas_copy(char *destination, int max, const char *source)
{
    int index = 0;

    while (source[index] != 0 && index < max - 1) {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = 0;
}

static char *kas_trim(char *text)
{
    char *end;

    while (*text == ' ' || *text == '\t') {
        ++text;
    }
    end = text + kas_strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t' ||
                          end[-1] == '\r' || end[-1] == '\n')) {
        --end;
        *end = 0;
    }
    return text;
}

static int kas_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* parses decimal, 0x hex, or 0b binary; returns false on anything else */
static bool kas_number(const char *text, int *out_value)
{
    bool negative = false;
    int base = 10;
    int value = 0;
    int digits = 0;

    if (*text == '-') {
        negative = true;
        ++text;
    } else if (*text == '+') {
        ++text;
    }
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text += 2;
    } else if (text[0] == '0' && (text[1] == 'b' || text[1] == 'B')) {
        base = 2;
        text += 2;
    }
    for (; *text != 0; ++text) {
        int digit = kas_digit(*text);

        if (digit < 0 || digit >= base) {
            return false;
        }
        value = value * base + digit;
        ++digits;
    }
    if (digits == 0) {
        return false;
    }
    *out_value = negative ? -value : value;
    return true;
}

/* non-destructive: returns a pointer past the number, or 0 if text does not
 * start with one.  Mirrors the host tool's strtol() based scan, so the string
 * that follows a run of numbers is still intact. */
static const char *kas_scan_number(const char *text, int *out_value)
{
    bool negative = false;
    int base = 10;
    int value = 0;
    int digits = 0;

    if (*text == '-') {
        negative = true;
        ++text;
    } else if (*text == '+') {
        ++text;
    }
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text += 2;
    } else if (text[0] == '0' && (text[1] == 'b' || text[1] == 'B')) {
        base = 2;
        text += 2;
    }
    for (; *text != 0; ++text) {
        int digit = kas_digit(*text);

        if (digit < 0 || digit >= base) {
            break;
        }
        value = value * base + digit;
        ++digits;
    }
    if (digits == 0) {
        return 0;
    }
    *out_value = negative ? -value : value;
    return text;
}

/* Label offsets and jump targets are relative to the program body, because
 * kby_load() strips the 8 byte header and starts pc at 0.  tools/kbasm does
 * the same, so the two assemblers agree byte for byte. */
static int kas_body_offset(void)
{
    return out_length - KAS_HEADER_SIZE;
}

static bool kas_put8(int value)
{
    if (out_length >= out_max) {
        kas_fail("program too large");
        return false;
    }
    out[out_length++] = (uint8_t)value;
    return true;
}

static bool kas_put16(int value)
{
    return kas_put8(value & 0xFF) && kas_put8((value >> 8) & 0xFF);
}

static bool kas_put32(int value)
{
    return kas_put8(value & 0xFF) && kas_put8((value >> 8) & 0xFF) &&
           kas_put8((value >> 16) & 0xFF) && kas_put8((value >> 24) & 0xFF);
}

static int kas_label_find(const char *name)
{
    for (int index = 0; index < label_count; ++index) {
        if (labels[index].used && kas_streq(labels[index].name, name)) {
            return index;
        }
    }
    return -1;
}

static int kas_label_define(const char *name)
{
    int index = kas_label_find(name);

    if (index >= 0) {
        kas_fail("duplicate label");
        return -1;
    }
    if (label_count >= KAS_LABELS) {
        kas_fail("too many labels");
        return -1;
    }
    index = label_count++;
    kas_copy(labels[index].name, KAS_LABEL_NAME, name);
    labels[index].target = (uint32_t)kas_body_offset();
    labels[index].used = true;
    return index;
}

/* splits the next whitespace-delimited token out of *cursor */
static char *kas_token(char **cursor)
{
    char *start = *cursor;
    char *scan = start;

    while (*scan == ' ' || *scan == '\t') {
        ++scan;
    }
    start = scan;
    while (*scan != 0 && *scan != ' ' && *scan != '\t') {
        ++scan;
    }
    if (*scan != 0) {
        *scan = 0;
        ++scan;
    }
    *cursor = scan;
    return start;
}

static bool kas_encode_string(const char *text, int max)
{
    uint8_t encoded[KAS_LINE];
    int length = 0;

    while (text[0] != 0 && length < max) {
        if (text[0] == '\\' && text[1] != 0) {
            ++text;
            if (text[0] == 'n') {
                encoded[length++] = '\n';
            } else if (text[0] == 't') {
                encoded[length++] = '\t';
            } else if (text[0] == 'r') {
                encoded[length++] = '\r';
            } else if (text[0] == '0') {
                encoded[length++] = 0;
            } else {
                encoded[length++] = (uint8_t)text[0];
            }
            ++text;
            continue;
        }
        encoded[length++] = (uint8_t)*text;
        ++text;
    }
    if (!kas_put16(length)) {
        return false;
    }
    for (int index = 0; index < length; ++index) {
        if (!kas_put8(encoded[index])) {
            return false;
        }
    }
    return true;
}

/* unquotes a token in place, matching tools/kbasm */
static char *kas_unquote(char *token)
{
    char *close;
    int length;

    if (*token != '"') {
        return token;
    }
    ++token;
    close = token + kas_strlen(token);
    while (close > token && close[-1] != '"') {
        --close;
    }
    if (close > token) {
        --close;
    }
    *close = 0;
    length = kas_strlen(token);
    if (length > 0 && token[length - 1] == ',') {
        token[length - 1] = 0;
    }
    return token;
}

/* gathers up to `max` numeric operands, stopping at the first non-number */
static int kas_numbers(char *cursor, int *values, int max)
{
    int count = 0;

    while (count < max) {
        char *token = kas_token(&cursor);
        int value;

        if (!kas_number(token, &value)) {
            return count;
        }
        values[count++] = value;
    }
    return count;
}

static void kas_line(char *line)
{
    char name[64];
    char *cursor;
    char *rest;
    int index = -1;
    const struct opdef *def = 0;
    int values[8];

    cursor = kas_trim(line);
    if (*cursor == 0 || *cursor == '#' || *cursor == ';') {
        return;
    }
    if (*cursor == '@') {
        kas_label_define(kas_trim(cursor + 1));
        return;
    }
    kas_copy(name, (int)sizeof(name), kas_token(&cursor));
    rest = kas_trim(cursor);
    if (kas_starts(name, ".include")) {
        return;
    }
    for (int scan = 0; scan < KBY_OPDEF_COUNT; ++scan) {
        if (kas_streq(kby_opcodes[scan].name, name)) {
            index = scan;
            def = &kby_opcodes[scan];
            break;
        }
    }
    if (def == 0) {
        kas_fail("unknown instruction");
        return;
    }
    if (!kas_put8(def->byte)) {
        return;
    }

    if (def->has_arg == 5) {
        char *target = rest;

        if (*target == '@') {
            ++target;
        }
        if (fixup_count >= KAS_FIXUPS) {
            kas_fail("too many label references");
            return;
        }
        if (kas_strlen(target) >= KAS_LABEL_NAME) {
            kas_fail("label name too long");
            return;
        }
        /* forward references are normal, so record the name and resolve it
         * after every line has been seen */
        fixups[fixup_count].where = out_length;
        /* value is body-relative; position stays file-absolute */
        kas_copy(fixups[fixup_count].name, KAS_LABEL_NAME, target);
        fixups[fixup_count].used = true;
        ++fixup_count;
        kas_put16(0);
        return;
    }
    if (def->has_arg == 4) {
        kas_encode_string(kas_unquote(rest), KAS_LINE);
        return;
    }
    if (def->has_arg == 7) {
        int count = kas_numbers(rest, values, 8);

        if (count < 3) {
            kas_fail("draw op needs at least 3 numbers");
            return;
        }
        for (index = 0; index < count; ++index) {
            kas_put32(values[index]);
        }
        return;
    }
    if (def->has_arg == 9) {
        int count = kas_numbers(rest, values, 8);

        if (count < 3) {
            kas_fail("draw op needs at least 3 numbers");
            return;
        }
        for (index = 0; index < count - 1; ++index) {
            kas_put32(values[index]);
        }
        kas_put8(values[count - 1]);
        return;
    }
    if (def->has_arg == 8) {
        /* leading numbers, then the rest of the line as the string */
        const char *scan = rest;
        const char *string_at;
        int count = 0;

        while (count < 8) {
            const char *stop;

            while (*scan == ' ' || *scan == '\t') {
                ++scan;
            }
            if (*scan == 0) {
                break;
            }
            stop = kas_scan_number(scan, &values[count]);
            if (stop == 0) {
                break;
            }
            scan = stop;
            ++count;
        }
        while (*scan == ' ' || *scan == '\t') {
            ++scan;
        }
        string_at = scan;
        if (!kas_put32(count)) {
            return;
        }
        for (index = 0; index < count; ++index) {
            kas_put32(values[index]);
        }
        kas_encode_string(kas_unquote((char *)string_at), KAS_LINE);
        return;
    }
    if (def->has_arg == 0) {
        return;
    }
    if (!kas_number(rest, &index)) {
        kas_fail("bad number");
        return;
    }
    if (def->has_arg == 1) {
        /* matches the host assembler: reject rather than truncate, and
         * reject negatives the same way it does */
        if (index < 0 || index > 255) {
            kas_fail("value does not fit in a byte");
            return;
        }
        kas_put8(index);
    } else if (def->has_arg == 3) {
        kas_put32(index);
    } else {
        kas_fail("bad operand encoding");
    }
}

static bool kas_emit_header(void)
{
    static const uint8_t magic[8] = { 'K', 'B', 'Y', '1', 1, 0, 0, 0 };

    for (int index = 0; index < 8; ++index) {
        if (!kas_put8(magic[index])) {
            return false;
        }
    }
    return true;
}

bool kas_assemble(const char *source, int source_length, uint8_t *out_buffer,
                  int max, int *length, const char **error)
{
    char line[KAS_LINE];
    int at = 0;

    labels[0].used = false;
    for (int index = 0; index < KAS_LABELS; ++index) {
        labels[index].used = false;
    }
    label_count = 0;
    for (int index = 0; index < KAS_FIXUPS; ++index) {
        fixups[index].used = false;
    }
    fixup_count = 0;
    out = out_buffer;
    out_length = 0;
    out_max = max;
    kas_error = 0;

    if (!kas_emit_header()) {
        if (error != 0) {
            *error = kas_error;
        }
        return false;
    }
    while (at < source_length) {
        int length = 0;

        while (at < source_length && source[at] != '\n' &&
               length < KAS_LINE - 1) {
            line[length++] = source[at++];
        }
        if (at < source_length && source[at] == '\n') {
            ++at;
        }
        line[length] = 0;
        kas_line(line);
        if (kas_error != 0) {
            if (error != 0) {
                *error = kas_error;
            }
            return false;
        }
    }
    for (int index = 0; index < fixup_count; ++index) {
        int label;
        int target;

        if (!fixups[index].used) {
            continue;
        }
        label = kas_label_find(fixups[index].name);
        if (label < 0) {
            if (error != 0) {
                *error = "undefined label";
            }
            return false;
        }
        target = (int)labels[label].target;
        out[fixups[index].where] = (uint8_t)(target & 0xFF);
        out[fixups[index].where + 1] = (uint8_t)((target >> 8) & 0xFF);
    }
    if (length != 0) {
        *length = out_length;
    }
    return true;
}
