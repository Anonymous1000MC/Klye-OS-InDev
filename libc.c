/* libc.c - the small freestanding C library the kernel and its guests need.
 *
 * Klye OS has no libc: mem.c provides the four mem* primitives and everything
 * else is hand rolled.  This file fills in the rest of what Lua 5.4 requires
 * (string.h, ctype.h, stdlib.h and a minimal snprintf), so a language runtime
 * can be linked into the kernel without dragging in a hosted toolchain.
 *
 * Everything here is freestanding-safe: no sbrk, no stdio streams, no locale.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------- size ---- */

size_t strlen(const char *text)
{
    size_t length = 0;

    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

int strcmp(const char *left, const char *right)
{
    while (*left != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return (int)(unsigned char)*left - (int)(unsigned char)*right;
}

int strncmp(const char *left, const char *right, size_t count)
{
    while (count > 0 && *left != '\0' && *left == *right) {
        ++left;
        ++right;
        --count;
    }
    if (count == 0) {
        return 0;
    }
    return (int)(unsigned char)*left - (int)(unsigned char)*right;
}

char *strcpy(char *destination, const char *source)
{
    size_t at = 0;

    while (source[at] != '\0') {
        destination[at] = source[at];
        ++at;
    }
    destination[at] = '\0';
    return destination;
}

char *strncpy(char *destination, const char *source, size_t count)
{
    size_t at = 0;

    while (at < count && source[at] != '\0') {
        destination[at] = source[at];
        ++at;
    }
    while (at < count) {
        destination[at] = '\0';
        ++at;
    }
    return destination;
}

char *strcat(char *destination, const char *source)
{
    size_t at = strlen(destination);

    while (source[0] != '\0') {
        destination[at++] = *source++;
    }
    destination[at] = '\0';
    return destination;
}

char *strchr(const char *text, int character)
{
    for (;; ++text) {
        if (*text == (char)character) {
            return (char *)(uintptr_t)text;
        }
        if (*text == '\0') {
            return 0;
        }
    }
}

char *strrchr(const char *text, int character)
{
    const char *found = 0;

    for (;; ++text) {
        if (*text == (char)character) {
            found = text;
        }
        if (*text == '\0') {
            break;
        }
    }
    return (char *)(uintptr_t)found;
}

char *strstr(const char *haystack, const char *needle)
{
    if (needle[0] == '\0') {
        return (char *)(uintptr_t)haystack;
    }
    for (; *haystack != '\0'; ++haystack) {
        const char *left = haystack;
        const char *right = needle;

        while (*left != '\0' && *left == *right) {
            ++left;
            ++right;
        }
        if (*right == '\0') {
            return (char *)(uintptr_t)haystack;
        }
    }
    return 0;
}

size_t strspn(const char *text, const char *accept)
{
    size_t length = 0;

    while (text[length] != '\0' && strchr(accept, text[length]) != 0) {
        ++length;
    }
    return length;
}

size_t strcspn(const char *text, const char *reject)
{
    size_t length = 0;

    while (text[length] != '\0' && strchr(reject, text[length]) == 0) {
        ++length;
    }
    return length;
}

char *strpbrk(const char *text, const char *accept)
{
    if (text[0] == '\0') {
        return 0;
    }
    for (; *text != '\0'; ++text) {
        if (strchr(accept, *text) != 0) {
            return (char *)(uintptr_t)text;
        }
    }
    return 0;
}

/* -------------------------------------------------------------- ctype ---- */

int isalpha(int character)
{
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z');
}

int isdigit(int character)
{
    return character >= '0' && character <= '9';
}

int isalnum(int character)
{
    return isalpha(character) || isdigit(character);
}

int isspace(int character)
{
    return character == ' ' || character == '\t' || character == '\n' ||
           character == '\v' || character == '\f' || character == '\r';
}

int isupper(int character)
{
    return character >= 'A' && character <= 'Z';
}

int islower(int character)
{
    return character >= 'a' && character <= 'z';
}

int isxdigit(int character)
{
    return isdigit(character) ||
           (character >= 'a' && character <= 'f') ||
           (character >= 'A' && character <= 'F');
}

int isprint(int character)
{
    return character >= 0x20 && character < 0x7F;
}

int iscntrl(int character)
{
    return (character >= 0 && character < 0x20) || character == 0x7F;
}

int ispunct(int character)
{
    return isprint(character) && !isalnum(character) && character != ' ';
}

int toupper(int character)
{
    if (islower(character)) {
        return character - 'a' + 'A';
    }
    return character;
}

int tolower(int character)
{
    if (isupper(character)) {
        return character - 'A' + 'a';
    }
    return character;
}

/* ------------------------------------------------------------- stdlib ---- */

int abs(int value)
{
    return value < 0 ? -value : value;
}

long labs(long value)
{
    return value < 0 ? -value : value;
}

static int libc_digit_value(int character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'z') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'Z') {
        return character - 'A' + 10;
    }
    return -1;
}

/* shared by strtol and strtoul; `negative_out` receives the sign */
static uint64_t libc_parse_integer(const char *text, char **end, int base,
                                   bool allow_negative, bool *negative_out)
{
    const char *scan = text;
    uint64_t value = 0;
    bool negative = false;
    bool any = false;

    while (isspace((unsigned char)*scan)) {
        ++scan;
    }
    if (*scan == '+' || (allow_negative && *scan == '-')) {
        negative = *scan == '-';
        ++scan;
    }
    if ((base == 0 || base == 16) && scan[0] == '0' &&
        (scan[1] == 'x' || scan[1] == 'X') && isxdigit((unsigned char)scan[2])) {
        scan += 2;
        base = 16;
    } else if (base == 0) {
        base = scan[0] == '0' ? 8 : 10;
    }
    for (;; ++scan) {
        int digit = libc_digit_value((unsigned char)*scan);

        if (digit < 0 || digit >= base) {
            break;
        }
        value = value * (uint64_t)base + (uint64_t)digit;
        any = true;
    }
    if (end != 0) {
        *end = (char *)(uintptr_t)(any ? scan : text);
    }
    if (negative_out != 0) {
        *negative_out = negative;
    }
    return value;
}

long strtol(const char *text, char **end, int base)
{
    bool negative = false;
    uint64_t value = libc_parse_integer(text, end, base, true, &negative);

    if (negative) {
        return -(long)value;
    }
    return (long)value;
}

unsigned long strtoul(const char *text, char **end, int base)
{
    bool negative = false;
    uint64_t value = libc_parse_integer(text, end, base, false, &negative);

    if (negative) {
        return (unsigned long)(-(long)value);
    }
    return (unsigned long)value;
}

int atoi(const char *text)
{
    return (int)strtol(text, 0, 10);
}

/* Lua parses every numeric literal through this, so it needs to be correct
 * for decimals, exponents, and both hex spellings. */
double strtod(const char *text, char **end)
{
    const char *scan = text;
    bool negative = false;
    double value = 0.0;
    double fraction = 0.1;
    bool any = false;

    while (isspace((unsigned char)*scan)) {
        ++scan;
    }
    if (*scan == '+' || *scan == '-') {
        negative = *scan == '-';
        ++scan;
    }
    if (scan[0] == '0' && (scan[1] == 'x' || scan[1] == 'X') &&
        isxdigit((unsigned char)scan[2])) {
        uint64_t hex = libc_parse_integer(scan, 0, 16, false, 0);

        if (end != 0) {
            const char *stop = scan;

            while (isxdigit((unsigned char)*stop)) {
                ++stop;
            }
            *end = (char *)(uintptr_t)stop;
        }
        return negative ? -(double)hex : (double)hex;
    }
    while (isdigit((unsigned char)*scan)) {
        value = value * 10.0 + (double)(*scan - '0');
        any = true;
        ++scan;
    }
    if (*scan == '.') {
        ++scan;
        while (isdigit((unsigned char)*scan)) {
            value += (double)(*scan - '0') * fraction;
            fraction *= 0.1;
            any = true;
            ++scan;
        }
    }
    if (any && (*scan == 'e' || *scan == 'E')) {
        const char *exponent = scan + 1;
        bool exp_negative = false;
        int power = 0;

        if (*exponent == '+' || *exponent == '-') {
            exp_negative = *exponent == '-';
            ++exponent;
        }
        if (isdigit((unsigned char)*exponent)) {
            while (isdigit((unsigned char)*exponent)) {
                if (power < 10000) {
                    power = power * 10 + (*exponent - '0');
                }
                ++exponent;
            }
            {
                double factor = 1.0;
                int steps = power;

                while (steps > 0) {
                    factor *= 10.0;
                    --steps;
                }
                value = exp_negative ? value / factor : value * factor;
            }
            scan = exponent;
        }
    }
    if (end != 0) {
        *end = (char *)(uintptr_t)(any ? scan : text);
    }
    return negative ? -value : value;
}

double atof(const char *text)
{
    return strtod(text, 0);
}

/* ----------------------------------------------------------- snprintf ---- */

struct libc_sink {
    char *buffer;
    size_t capacity;
    size_t used;
};

static void sink_putc(struct libc_sink *sink, char value)
{
    if (sink->capacity != 0 && sink->used + 1 < sink->capacity) {
        sink->buffer[sink->used] = value;
    }
    ++sink->used;
}

static void sink_puts(struct libc_sink *sink, const char *text)
{
    while (*text != '\0') {
        sink_putc(sink, *text++);
    }
}

static void sink_pad(struct libc_sink *sink, char pad, int count)
{
    for (int index = 0; index < count; ++index) {
        sink_putc(sink, pad);
    }
}

/* renders an unsigned value in the given base; returns characters written */
static int sink_number(struct libc_sink *sink, uint64_t value, unsigned base,
                       bool upper, int width, int precision, bool negative,
                       bool left_align, bool zero_pad, bool plus,
                       bool space_pad)
{
    static const char lower_digits[] = "0123456789abcdef";
    static const char upper_digits[] = "0123456789ABCDEF";
    const char *table = upper ? upper_digits : lower_digits;
    char reversed[24];
    int length = 0;
    int prefix = 0;
    int body;

    if (value == 0) {
        reversed[length++] = '0';
    }
    while (value != 0 && length < (int)sizeof(reversed)) {
        reversed[length++] = table[value % base];
        value /= base;
    }
    if (precision > length) {
        length = precision;
    }
    body = length;
    if (negative) {
        prefix = 1;
    } else if (plus) {
        prefix = 1;
    } else if (space_pad) {
        prefix = 1;
    }
    if (width < body + prefix) {
        width = body + prefix;
    }
    if (!left_align && !zero_pad) {
        sink_pad(sink, ' ', width - body - prefix);
    }
    if (negative) {
        sink_putc(sink, '-');
    } else if (plus) {
        sink_putc(sink, '+');
    } else if (space_pad) {
        sink_putc(sink, ' ');
    }
    if (!left_align && zero_pad) {
        sink_pad(sink, '0', width - body - prefix);
    }
    for (int index = length - 1; index >= 0; --index) {
        if (precision > 0 && index >= precision) {
            continue;
        }
        sink_putc(sink, index < (int)strlen(reversed)
                          ? reversed[index]
                          : '0');
    }
    if (left_align) {
        sink_pad(sink, ' ', width - body - prefix);
    }
    return body + prefix + (width > body + prefix ? width - body - prefix : 0);
}

int vsnprintf(char *buffer, size_t size, const char *format, va_list arguments)
{
    struct libc_sink sink;

    sink.buffer = buffer;
    sink.capacity = size;
    sink.used = 0;
    while (*format != '\0') {
        bool left_align = false;
        bool zero_pad = false;
        bool plus = false;
        bool space_pad = false;
        int width = 0;
        int precision = -1;
        int length_modifier = 0; /* 1 = long, 2 = long long, -1 = short */
        char conversion;

        if (*format != '%') {
            sink_putc(&sink, *format++);
            continue;
        }
        ++format;
        if (*format == '%') {
            sink_putc(&sink, *format++);
            continue;
        }
        for (;;) {
            if (*format == '-') {
                left_align = true;
            } else if (*format == '0') {
                zero_pad = true;
            } else if (*format == '+') {
                plus = true;
            } else if (*format == ' ') {
                space_pad = true;
            } else {
                break;
            }
            ++format;
        }
        if (*format == '*') {
            width = va_arg(arguments, int);
            if (width < 0) {
                left_align = true;
                width = -width;
            }
            ++format;
        } else {
            while (isdigit((unsigned char)*format)) {
                width = width * 10 + (*format - '0');
                ++format;
            }
        }
        if (*format == '.') {
            ++format;
            precision = 0;
            if (*format == '*') {
                precision = va_arg(arguments, int);
                ++format;
            } else {
                while (isdigit((unsigned char)*format)) {
                    precision = precision * 10 + (*format - '0');
                    ++format;
                }
            }
        }
        for (;;) {
            if (*format == 'l') {
                length_modifier = length_modifier == 1 ? 2 : 1;
            } else if (*format == 'h') {
                length_modifier = -1;
            } else if (*format == 'z' || *format == 'j' || *format == 't') {
                length_modifier = 1;
            } else {
                break;
            }
            ++format;
        }
        conversion = *format;
        if (conversion == '\0') {
            break;
        }
        ++format;
        switch (conversion) {
        case 'd':
        case 'i': {
            int64_t value = length_modifier == 0
                                ? (int64_t)va_arg(arguments, int)
                                : va_arg(arguments, long long);

            if (value < 0) {
                sink_number(&sink, (uint64_t)(-value), 10, false, width,
                            precision, true, left_align, zero_pad, plus,
                            space_pad);
            } else {
                sink_number(&sink, (uint64_t)value, 10, false, width,
                            precision, false, left_align, zero_pad, plus,
                            space_pad);
            }
            break;
        }
        case 'u': {
            uint64_t value = length_modifier == 0
                                 ? (uint64_t)va_arg(arguments, unsigned int)
                                 : (uint64_t)va_arg(arguments, unsigned long long);

            sink_number(&sink, value, 10, false, width, precision, false,
                        left_align, zero_pad, false, false);
            break;
        }
        case 'x':
        case 'X':
        case 'o':
        case 'b': {
            uint64_t value = length_modifier == 0
                                 ? (uint64_t)va_arg(arguments, unsigned int)
                                 : (uint64_t)va_arg(arguments, unsigned long long);
            unsigned base = conversion == 'o' ? 8U
                             : conversion == 'b' ? 2U
                                                 : 16U;

            sink_number(&sink, value, base, conversion == 'X', width, precision,
                        false, left_align, zero_pad, false, false);
            break;
        }
        case 'p': {
            uint64_t value = (uint64_t)(uintptr_t)va_arg(arguments, void *);

            sink_puts(&sink, "0x");
            sink_number(&sink, value, 16, false, width, -1, false, left_align,
                        true, false, false);
            break;
        }
        case 'c': {
            char value = (char)va_arg(arguments, int);

            sink_pad(&sink, ' ', left_align ? 0 : (width > 1 ? width - 1 : 0));
            sink_putc(&sink, value);
            break;
        }
        case 's': {
            const char *text = va_arg(arguments, const char *);

            if (text == 0) {
                text = "(null)";
            }
            if (!left_align) {
                sink_pad(&sink, ' ', width > (int)strlen(text)
                                      ? width - (int)strlen(text)
                                      : 0);
            }
            sink_puts(&sink, text);
            if (left_align) {
                sink_pad(&sink, ' ', width > (int)strlen(text)
                                      ? width - (int)strlen(text)
                                      : 0);
            }
            break;
        }
        case 'f':
        case 'F':
        case 'g':
        case 'G':
        case 'e':
        case 'E': {
            /* Build the number in a scratch buffer first.  Padding has to be
             * computed from the finished length, otherwise zero padding
             * would treat the integer part as the whole number. */
            char scratch[400];
            struct libc_sink inner;
            double value = va_arg(arguments, double);
            double magnitude = value < 0.0 ? -value : value;
            int places = precision >= 0 ? precision : 6;
            double scale = 1.0;
            uint64_t scaled;
            uint64_t whole;
            uint64_t part;
            int body;

            if (places > 18) {
                places = 18;
            }
            if (places < 0) {
                places = 0;
            }
            for (int index = 0; index < places; ++index) {
                scale *= 10.0;
            }
            scaled = (uint64_t)(magnitude * scale + 0.5);
            whole = places > 0 ? scaled / (uint64_t)scale : scaled;
            part = places > 0 ? scaled % (uint64_t)scale : 0;

            inner.buffer = scratch;
            inner.capacity = sizeof(scratch);
            inner.used = 0;
            if (value < 0.0 && (whole != 0 || part != 0)) {
                sink_putc(&inner, '-');
            } else if (value >= 0.0 && plus) {
                sink_putc(&inner, '+');
            } else if (value >= 0.0 && space_pad) {
                sink_putc(&inner, ' ');
            }
            sink_number(&inner, whole, 10, false, 0, -1, false, false, false,
                        false, false);
            if (places > 0) {
                sink_putc(&inner, '.');
                for (int index = places - 1; index >= 0; --index) {
                    uint64_t divisor = 1;

                    for (int step = 0; step < index; ++step) {
                        divisor *= 10;
                    }
                    sink_putc(&inner, (char)('0' + (part / divisor) % 10));
                }
            }
            if (conversion == 'e' || conversion == 'E') {
                int exponent = 0;
                double probe = magnitude;

                while (probe >= 10.0) {
                    probe /= 10.0;
                    ++exponent;
                }
                while (probe != 0.0 && probe < 1.0) {
                    probe *= 10.0;
                    --exponent;
                }
                sink_putc(&inner, conversion == 'e' ? 'e' : 'E');
                sink_putc(&inner, exponent < 0 ? '-' : '+');
                {
                    int size = exponent < 0 ? -exponent : exponent;

                    if (size >= 100) {
                        sink_putc(&inner, (char)('0' + size / 100));
                    }
                    if (size >= 10) {
                        sink_putc(&inner, (char)('0' + (size / 10) % 10));
                        sink_putc(&inner, (char)('0' + size % 10));
                    } else {
                        sink_putc(&inner, '0');
                        sink_putc(&inner, (char)('0' + size));
                    }
                }
            }
            body = (int)inner.used;
            if (!left_align && width > body) {
                sink_pad(&sink, zero_pad ? '0' : ' ', width - body);
            }
            for (int index = 0; index < body; ++index) {
                sink_putc(&sink, scratch[index]);
            }
            if (left_align) {
                sink_pad(&sink, ' ', width - body);
            }
            break;
        }
        default:
            sink_putc(&sink, '%');
            sink_putc(&sink, conversion);
            break;
        }
    }
    if (size != 0) {
        size_t end = sink.used < size - 1 ? sink.used : size - 1;

        buffer[end] = '\0';
    }
    return (int)sink.used;
}

int snprintf(char *buffer, size_t size, const char *format, ...)
{
    va_list arguments;
    int written;

    va_start(arguments, format);
    written = vsnprintf(buffer, size, format, arguments);
    va_end(arguments);
    return written;
}

int sprintf(char *buffer, const char *format, ...)
{
    va_list arguments;
    int written;

    va_start(arguments, format);
    written = vsnprintf(buffer, (size_t)-1, format, arguments);
    va_end(arguments);
    return written;
}

/* Lua's error paths call these; there is no process to exit, so they just
 * stop.  A caller that must survive can install its own hook via
 * libc_set_panic(). */
static void (*libc_panic_hook)(const char *message);

void libc_set_panic(void (*hook)(const char *message))
{
    libc_panic_hook = hook;
}

void abort(void)
{
    if (libc_panic_hook != 0) {
        libc_panic_hook("abort() called");
    }
    for (;;) {
        __asm__ volatile("hlt");
    }
}

void exit(int status)
{
    if (libc_panic_hook != 0) {
        libc_panic_hook("exit() called");
    }
    (void)status;
    for (;;) {
        __asm__ volatile("hlt");
    }
}

void qsort(void *base, size_t count, size_t size,
           int (*compare)(const void *, const void *))
{
    /* insertion sort: no allocation, and the call sites are tiny */
    for (size_t outer = 1; outer < count; ++outer) {
        for (size_t inner = outer; inner > 0; --inner) {
            char *left = (char *)base + (inner - 1) * size;
            char *right = (char *)base + inner * size;
            char swap[64];
            bool large = size > sizeof(swap);

            if (compare(left, right) <= 0) {
                break;
            }
            if (large) {
                for (size_t byte = 0; byte < size; ++byte) {
                    char temp = left[byte];

                    left[byte] = right[byte];
                    right[byte] = temp;
                }
            } else {
                for (size_t byte = 0; byte < size; ++byte) {
                    swap[byte] = left[byte];
                    left[byte] = right[byte];
                    right[byte] = swap[byte];
                }
            }
        }
    }
}

char *getenv(const char *name)
{
    (void)name;
    return 0;
}
