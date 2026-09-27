/* kbasm - assemble Klye bytecode (.kby) into a KBY1 image (.OUTNAME).
 *
 * Host tool. Two passes: pass one records label positions, pass two emits.
 * A label reference to a not-yet-defined label is patched in pass two, so
 * forward jumps work.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SOURCE 262144
#define MAX_LABELS 256
#define MAX_PATCH 512
#define MAX_LINE 512

struct label {
    char name[64];
    long offset;
};

struct patch {
    long at;
    char name[64];
};

struct kbasm {
    unsigned char code[MAX_SOURCE];
    long length;
    struct label labels[MAX_LABELS];
    int label_count;
    struct patch patches[MAX_PATCH];
    int patch_count;
    char error[160];
    char include_name[160];
    unsigned char include_buf[MAX_SOURCE];
    long include_length;
};

static struct kbasm kb;

static void fail(const char *fmt, const char *detail)
{
    snprintf(kb.error, sizeof(kb.error), fmt, detail);
}

static void fail2(const char *fmt, const char *first, const char *second)
{
    snprintf(kb.error, sizeof(kb.error), fmt, first, second);
}

static unsigned char *where(long at)
{
    if (at < 0 || at >= (long)sizeof(kb.code)) {
        fail("offset %s out of range", "out of range");
        return 0;
    }
    return &kb.code[at];
}

static void put8(unsigned value)
{
    unsigned char *slot;

    if (kb.length >= (long)sizeof(kb.code) - 8) {
        fail("program too large: %s", "limit reached");
        return;
    }
    slot = where(kb.length);
    if (slot == 0) {
        return;
    }
    *slot = (unsigned char)(value & 0xFF);
    ++kb.length;
}

static void put16(unsigned value)
{
    put8(value & 0xFF);
    put8((value >> 8) & 0xFF);
}

static void put32(uint32_t value)
{
    put8(value & 0xFF);
    put8((value >> 8) & 0xFF);
    put8((value >> 16) & 0xFF);
    put8((value >> 24) & 0xFF);
}

static void define_label(const char *name)
{
    if (strlen(name) >= sizeof(kb.labels[0].name)) {
        fail("label name too long: %.32s", name);
        return;
    }
    if (kb.label_count >= MAX_LABELS) {
        fail("too many labels: %s", name);
        return;
    }
    snprintf(kb.labels[kb.label_count].name,
             sizeof(kb.labels[kb.label_count].name), "%s", name);
    kb.labels[kb.label_count].offset = kb.length;
    ++kb.label_count;
}

static void patch_label(const char *name)
{
    if (strlen(name) >= sizeof(kb.patches[0].name)) {
        fail("label name too long: %.32s", name);
        return;
    }
    if (kb.patch_count >= MAX_PATCH) {
        fail("too many label references: %s", name);
        return;
    }
    kb.patches[kb.patch_count].at = kb.length;
    snprintf(kb.patches[kb.patch_count].name,
             sizeof(kb.patches[kb.patch_count].name), "%s", name);
    ++kb.patch_count;
    put16(0);
}

static long lookup_label(const char *name)
{
    for (int index = 0; index < kb.label_count; ++index) {
        if (strcmp(kb.labels[index].name, name) == 0) {
            return kb.labels[index].offset;
        }
    }
    return -1;
}

/* has_arg: 0 none, 1 eight, 2 sixteen, 3 thirtytwo, 4 string, 5 label */
struct opdef {
    const char *name;
    int has_arg;
    unsigned char byte;
};

static const struct opdef opcodes[] = {
    { ".nop", 0, 0x00 },
    { ".push8", 1, 0x01 },
    { ".push32", 3, 0x02 },
    { ".pushstr", 4, 0x03 },
    { ".pop", 0, 0x04 },
    { ".dup", 0, 0x05 },
    { ".load", 1, 0x06 },
    { ".store", 1, 0x07 },
    { ".add", 0, 0x08 },
    { ".sub", 0, 0x09 },
    { ".mul", 0, 0x0A },
    { ".div", 0, 0x0B },
    { ".mod", 0, 0x0C },
    { ".cmp", 0, 0x65 },
    { ".jmp", 5, 0x0D },
    { ".jz", 5, 0x0E },
    { ".jnz", 5, 0x0F },
    { ".call", 5, 0x10 },
    { ".ret", 0, 0x11 },
    { ".halt", 0, 0x12 },
    { ".print", 0, 0x20 },
    { ".println", 0, 0x21 },
    { ".clear", 0, 0x30 },
    { ".rect", 7, 0x31 },
    { ".rounded", 9, 0x32 },
    { ".border", 9, 0x33 },
    { ".pixel", 0, 0x34 },
    { ".circle", 0, 0x35 },
    { ".line", 0, 0x36 },
    { ".text", 8, 0x37 },
    { ".textc", 8, 0x38 },
    { ".vfs_exists", 0, 0x50 },
    { ".vfs_size", 0, 0x51 },
    { ".vfs_read", 0, 0x52 },
    { ".vfs_write", 0, 0x53 },
    { ".vfs_append", 0, 0x54 },
    { ".ticks", 0, 0x60 },
    { ".num", 0, 0x66 },
    { 0, 0, 0 }
};

static char *trim(char *text)
{
    char *end;
    char *hash;

    hash = strchr(text, '#');
    if (hash != 0) {
        *hash = 0;
    }
    while (*text == ' ' || *text == '\t') {
        ++text;
    }
    end = text + strlen(text);
    while (end > text &&
           (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' ||
            end[-1] == '\n')) {
        --end;
    }
    *end = 0;
    return text;
}

static long parse_number(const char *text)
{
    char *stop = 0;
    long value;

    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        return strtol(text + 2, &stop, 16);
    }
    if (text[0] == '$') {
        return strtol(text + 1, &stop, 16);
    }
    value = strtol(text, &stop, 10);
    if (stop != 0 && *stop != 0) {
        return -1;
    }
    return value;
}

static void assemble_line(char *line)
{
    char name[64];
    char rest[MAX_LINE];
    char *cursor;
    int index;
    const struct opdef *def = 0;
    long value;

    cursor = trim(line);
    if (cursor[0] == 0 || cursor[0] == '#' || cursor[0] == ';') {
        return;
    }
    if (cursor[0] == '@') {
        define_label(trim(cursor + 1));
        return;
    }
    if (strcmp(cursor, ".include") == 0) {
        return;
    }

    index = 0;
    while (cursor[index] != 0 && cursor[index] != ' ' &&
           cursor[index] != '\t' && index < (int)sizeof(name) - 1) {
        name[index] = cursor[index];
        ++index;
    }
    name[index] = 0;
    rest[0] = 0;
    {
        char *tail = trim(cursor + index);

        snprintf(rest, sizeof(rest), "%s", tail);
    }

    for (index = 0; opcodes[index].name != 0; ++index) {
        if (strcmp(opcodes[index].name, name) == 0) {
            def = &opcodes[index];
            break;
        }
    }
    if (def == 0) {
        fail("unknown instruction '%s'", name);
        return;
    }
    put8((unsigned)def->byte);

    if (def->has_arg == 0) {
        return;
    }
    if (def->has_arg == 5) {
        const char *target = rest;

        if (target[0] == '@') {
            ++target;
        }
        patch_label(target);
        return;
    }
    if (def->has_arg == 4) {
        char *literal = rest;

        if (literal[0] == '"') {
            char *close = strrchr(literal + 1, '"');

            if (close == 0) {
                fail("unterminated string on: %s", name);
                return;
            }
            *close = 0;
            ++literal;
        }
        {
            unsigned char encoded[MAX_LINE];
            size_t out = 0;

            for (size_t at = 0; literal[at] != 0 && out < sizeof(encoded) - 1;
                 ++at) {
                if (literal[at] == '\\' && literal[at + 1] != 0) {
                    ++at;
                    if (literal[at] == 'n') {
                        encoded[out++] = '\n';
                    } else if (literal[at] == 't') {
                        encoded[out++] = '\t';
                    } else if (literal[at] == 'r') {
                        encoded[out++] = '\r';
                    } else if (literal[at] == '0') {
                        encoded[out++] = 0;
                    } else {
                        encoded[out++] = (unsigned char)literal[at];
                    }
                } else {
                    encoded[out++] = (unsigned char)literal[at];
                }
            }
            put16((unsigned)out);
            for (size_t at = 0; at < out; ++at) {
                put8(encoded[at]);
            }
        }
        return;
    }
    if (def->has_arg == 7 || def->has_arg == 9) {
        /* draw op: all operands are numbers, emitted left to right as 32-bit
         * values except a single trailing 8-bit literal (radius) */
        char *save = rest;
        char *tok = rest;
        int count = 0;

        while (*tok != 0) {
            ++count;
            while (*tok == ' ') {
                ++tok;
            }
            while (*tok != 0 && *tok != ' ') {
                ++tok;
            }
        }
        if (count < 3) {
            fail("draw op '%.32s' needs at least 3 numbers", name);
            return;
        }
        {
            int index = 0;
            long numbers[8];
            char *scan = rest;

            while (*scan != 0 && index < 8) {
                char *stop = 0;
                long got;

                while (*scan == ' ') {
                    ++scan;
                }
                if (*scan == 0) {
                    break;
                }
                got = strtol(scan, &stop, 0);
                if (stop == scan) {
                    break;
                }
                numbers[index++] = got;
                scan = stop;
            }
            if (def->has_arg == 9) {
                for (index = 0; index < count - 1; ++index) {
                    put32((uint32_t)numbers[index]);
                }
                put8((unsigned)numbers[count - 1]);
            } else {
                for (index = 0; index < count; ++index) {
                    put32((uint32_t)numbers[index]);
                }
            }
        }
        (void)save;
        return;
    }
    if (def->has_arg == 8) {
        /* .text: trailing string first, then numeric operands */
        char *string_at;
        char *scan = rest;
        long numbers[8];
        int count = 0;
        int index;
        unsigned char encoded[MAX_LINE];
        size_t out = 0;
        size_t length;

        /* take only leading tokens that are entirely numeric; the first
         * non-numeric token starts the string */
        while (*scan != 0 && count < 8) {
            char *stop = 0;
            long got;

            while (*scan == ' ') {
                ++scan;
            }
            if (*scan == 0) {
                break;
            }
            got = strtol(scan, &stop, 0);
            if (stop == scan) {
                break;
            }
            numbers[count++] = got;
            scan = stop;
        }
        string_at = scan;
        if (*string_at == '"') {
            char *close = strrchr(string_at + 1, '"');

            if (close == 0) {
                fail("unterminated string on: %s", name);
                return;
            }
            *close = 0;
            ++string_at;
        }
        for (index = 0; string_at[index] != 0 && (size_t)index < sizeof(encoded) - 1;
             ++index) {
            if (string_at[index] == '\\' && string_at[index + 1] != 0) {
                ++index;
                if (string_at[index] == 'n') {
                    encoded[out++] = '\n';
                } else {
                    encoded[out++] = (unsigned char)string_at[index];
                }
            } else {
                encoded[out++] = (unsigned char)string_at[index];
            }
        }
        length = out;
        put32((uint32_t)count);
        for (index = 0; index < count; ++index) {
            put32((uint32_t)numbers[index]);
        }
        put16((unsigned)length);
        for (index = 0; (size_t)index < length; ++index) {
            put8(encoded[index]);
        }
        return;
    }
    value = parse_number(rest);
    if (value < 0) {
        fail2("bad number '%s' for: %s", rest, name);
        return;
    }
    if (def->has_arg == 1) {
        put8((unsigned)value);
    } else if (def->has_arg == 3) {
        put32((uint32_t)value);
    }
}

static int assemble_buffer(char *text, long length)
{
    long at = 0;

    while (at < length) {
        long start = at;
        long stop;

        while (at < length && text[at] != '\n') {
            ++at;
        }
        stop = at;
        if (stop >= length) {
            stop = length;
        }
        {
            char line[MAX_LINE];
            long span = stop - start;

            if (span >= (long)sizeof(line)) {
                span = (long)sizeof(line) - 1;
            }
            memcpy(line, text + start, (size_t)span);
            line[span] = 0;
            assemble_line(line);
        }
        if (kb.error[0] != 0) {
            return 0;
        }
        ++at;
    }
    return 1;
}

int main(int argc, char **argv)
{
    FILE *in;
    FILE *out;
    long length;
    char source[MAX_SOURCE];

    if (argc < 2) {
        fprintf(stderr, "usage: kbasm <file.kby> [out.kbin]\n"
                "  labels are written as @name\n");
        return 2;
    }
    memset(&kb, 0, sizeof(kb));

    in = fopen(argv[1], "rb");
    if (in == 0) {
        fprintf(stderr, "kbasm: cannot open %s\n", argv[1]);
        return 1;
    }
    length = (long)fread(source, 1, sizeof(source), in);
    fclose(in);
    if (length >= (long)sizeof(source)) {
        fprintf(stderr, "kbasm: %s is too large\n", argv[1]);
        return 1;
    }
    source[length] = 0;

    /* pass 1: code plus labels */
    if (!assemble_buffer(source, length)) {
        fprintf(stderr, "kbasm: %s\n", kb.error);
        return 1;
    }

    /* pass 2: resolve label references */
    for (int index = 0; index < kb.patch_count; ++index) {
        long target = lookup_label(kb.patches[index].name);

        if (target < 0) {
            fprintf(stderr, "kbasm: undefined label '%s'\n",
                    kb.patches[index].name);
            return 1;
        }
        kb.code[kb.patches[index].at] = (unsigned char)(target & 0xFF);
        kb.code[kb.patches[index].at + 1] =
            (unsigned char)((target >> 8) & 0xFF);
    }

    out = argc > 2 ? fopen(argv[2], "wb") : stdout;
    if (out == 0) {
        fprintf(stderr, "kbasm: cannot write output\n");
        return 1;
    }
    {
        unsigned char header[8] = { 'K', 'B', 'Y', '1', 1, 0, 0, 0 };

        fwrite(header, 1, sizeof(header), out);
    }
    fwrite(kb.code, 1, (size_t)kb.length, out);
    if (out != stdout) {
        fclose(out);
    }
    fprintf(stderr, "kbasm: %s -> %ld bytes, %d labels\n", argv[1],
            kb.length, kb.label_count);
    return 0;
}
