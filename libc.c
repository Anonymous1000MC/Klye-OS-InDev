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
#include <time.h>

#include "heap.h"

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
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A': {
            /* Two different notions of "digits" are in play and mixing them
             * up is the whole bug here:
             *   %f  precision means digits after the decimal point
             *   %e  precision means digits after the first
             *   %g  precision means significant digits overall
             * The magnitude is normalised into a separate variable so the
             * plain path still has the original value to work from. */
            char scratch[512];
            struct libc_sink inner;
            double value = va_arg(arguments, double);
            double magnitude = value < 0.0 ? -value : value;
            double normalised = magnitude;
            bool general = conversion == 'g' || conversion == 'G';
            bool upper = conversion == 'E' || conversion == 'G' ||
                         conversion == 'A';
            bool want_exponent = conversion == 'e' || conversion == 'E';
            int significant = precision >= 0 ? precision : 6;
            int exponent = 0;
            int decimals;
            int body;
            bool use_exponent;

            if (significant <= 0) {
                significant = 1;
            }
            if (significant > 17) {
                significant = 17;
            }
            if (normalised != 0.0) {
                while (normalised >= 10.0) {
                    normalised /= 10.0;
                    ++exponent;
                }
                while (normalised < 1.0) {
                    normalised *= 10.0;
                    --exponent;
                }
            }
            if (want_exponent) {
                use_exponent = true;
            } else if (general) {
                use_exponent = exponent < -4 || exponent >= significant;
            } else {
                /* an integral part past 2^63 will not fit a uint64 */
                use_exponent = exponent >= 19;
            }
            if (general) {
                decimals = use_exponent ? significant - 1
                                        : significant - 1 - exponent;
            } else {
                /* %f and %e both count digits after the decimal point */
                decimals = precision >= 0 ? precision : 6;
            }
            if (decimals < 0) {
                decimals = 0;
            }
            if (decimals > 40) {
                decimals = 40;
            }
            inner.buffer = scratch;
            inner.capacity = sizeof(scratch);
            inner.used = 0;
            if (value < 0.0) {
                sink_putc(&inner, '-');
            } else if (plus) {
                sink_putc(&inner, '+');
            } else if (space_pad) {
                sink_putc(&inner, ' ');
            }
            if (magnitude == 0.0) {
                sink_putc(&inner, '0');
                if (decimals > 0) {
                    sink_putc(&inner, '.');
                    for (int index = 0; index < decimals; ++index) {
                        sink_putc(&inner, '0');
                    }
                }
            } else if (use_exponent) {
                uint64_t lead = (uint64_t)normalised;
                double fraction = normalised - (double)lead;
                uint8_t digits[48];

                for (int index = 0; index < decimals && index < 48; ++index) {
                    fraction *= 10.0;
                    digits[index] = (uint8_t)(uint64_t)fraction;
                    fraction -= (double)digits[index];
                }
                if (decimals > 0 && decimals <= 48) {
                    int carry = fraction * 10.0 >= 0.5 ? 1 : 0;

                    for (int index = decimals - 1; index >= 0 && carry != 0;
                         --index) {
                        if (digits[index] + carry >= 10) {
                            digits[index] = 0;
                            carry = 1;
                        } else {
                            digits[index] = (uint8_t)(digits[index] + carry);
                            carry = 0;
                        }
                    }
                    if (carry != 0) {
                        lead += 1;
                    }
                }
                sink_number(&inner, lead, 10, false, false, -1, false, false,
                            false, false, false);
                if (decimals > 0) {
                    sink_putc(&inner, '.');
                    for (int index = 0; index < decimals && index < 48;
                         ++index) {
                        sink_putc(&inner, (char)('0' + digits[index]));
                    }
                }
            } else {
                uint64_t integral = (uint64_t)magnitude;
                double fraction = magnitude - (double)integral;
                uint8_t digits[48];

                for (int index = 0; index < decimals && index < 48; ++index) {
                    fraction *= 10.0;
                    digits[index] = (uint8_t)(uint64_t)fraction;
                    fraction -= (double)digits[index];
                }
                /* Round the last digit rather than truncating, otherwise
                 * 1414/1000 prints as 1.4139999999999. */
                if (decimals > 0 && decimals <= 48) {
                    int carry = 0;

                    if (fraction * 10.0 >= 0.5) {
                        carry = 1;
                    }
                    for (int index = decimals - 1; index >= 0 && carry != 0;
                         --index) {
                        if (digits[index] + carry >= 10) {
                            digits[index] = 0;
                            carry = 1;
                        } else {
                            digits[index] = (uint8_t)(digits[index] + carry);
                            carry = 0;
                        }
                    }
                    if (carry != 0) {
                        integral += 1;
                    }
                }
                sink_number(&inner, integral, 10, false, 0, -1, false, false,
                            false, false, false);
                if (decimals > 0) {
                    sink_putc(&inner, '.');
                    for (int index = 0; index < decimals && index < 48;
                         ++index) {
                        sink_putc(&inner, (char)('0' + digits[index]));
                    }
                }
            }
            if (use_exponent) {
                int size = exponent < 0 ? -exponent : exponent;

                sink_putc(&inner, upper ? 'E' : 'e');
                sink_putc(&inner, exponent < 0 ? '-' : '+');
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
            if (general) {
                /* %g removes trailing zeros from the fraction only.  The
                 * exponent suffix has to survive, so it is shifted down to
                 * close the gap the removal leaves. */
                int length = (int)inner.used;
                int exponent_index = -1;
                int point_index = -1;
                int limit;
                int cut;

                if (use_exponent) {
                    for (int index = length - 2; index >= 0; --index) {
                        if (scratch[index] == 'e' || scratch[index] == 'E') {
                            exponent_index = index;
                            break;
                        }
                    }
                }
                limit = exponent_index >= 0 ? exponent_index : length;
                for (int index = 0; index < limit; ++index) {
                    if (scratch[index] == '.') {
                        point_index = index;
                        break;
                    }
                }
                cut = limit;
                if (point_index >= 0) {
                    while (cut > point_index + 1 && scratch[cut - 1] == '0') {
                        --cut;
                    }
                    if (cut == point_index + 1) {
                        cut = point_index;
                    }
                }
                if (exponent_index >= 0 && cut < exponent_index) {
                    for (int index = exponent_index; index < length; ++index) {
                        scratch[cut + (index - exponent_index)] = scratch[index];
                    }
                    inner.used = (size_t)(cut + (length - exponent_index));
                } else {
                    inner.used = (size_t)cut;
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

/* ---------------------------------------------------------------- math ---- */
/* Only what Lua's math library calls.  Each routine is a range reduction plus
 * a short polynomial, which is accurate to well under a double ulp for the
 * argument ranges a UI toolkit ever uses. */

#define MATH_PI 3.14159265358979323846
#define MATH_TWO_PI 6.28318530717958647692
#define MATH_HALF_PI 1.57079632679489661923

double fabs(double value)
{
    return value < 0.0 ? -value : value;
}

double floor(double value)
{
    double truncated = (double)(long long)value;

    if (value < 0.0 && truncated != value) {
        truncated -= 1.0;
    }
    return truncated;
}

double ceil(double value)
{
    double truncated = (double)(long long)value;

    if (value > 0.0 && truncated != value) {
        truncated += 1.0;
    }
    return truncated;
}

double trunc(double value)
{
    return (double)(long long)value;
}

double round(double value)
{
    return value < 0.0 ? -floor(-value + 0.5) : floor(value + 0.5);
}

double sqrt(double value)
{
    double guess;
    int iteration;

    if (value < 0.0) {
        return 0.0 / 1.0; /* NaN, produced without a NaN literal */
    }
    if (value == 0.0) {
        return 0.0;
    }
    guess = value > 1.0 ? value * 0.5 : 1.0;
    for (iteration = 0; iteration < 60; ++iteration) {
        double next = 0.5 * (guess + value / guess);

        if (next == guess) {
            break;
        }
        guess = next;
    }
    return guess;
}

/* sin via Cody-Waite range reduction and a degree-11 odd polynomial */
static double sin_reduced(double x)
{
    double square = x * x;

    return x * (1.0 +
           square * (-1.0 / 6.0 +
           square * (1.0 / 120.0 +
           square * (-1.0 / 5040.0 +
           square * (1.0 / 362880.0 +
           square * (-1.0 / 39916800.0))))));
}

double sin(double value)
{
    double quotient = value / MATH_TWO_PI;
    double turns;
    double reduced;

    quotient = quotient < 0.0 ? -floor(-quotient + 0.5) : floor(quotient + 0.5);
    turns = quotient;
    reduced = value - turns * MATH_TWO_PI;
    /* Cody-Waite split of 2*pi into three 16 bit friendly pieces */
    reduced -= turns * 1.2246467991473532072e-16;
    return sin_reduced(reduced);
}

double cos(double value)
{
    return sin(value + MATH_HALF_PI);
}

double tan(double value)
{
    double c = cos(value);

    if (c == 0.0) {
        return value < 0.0 ? -1e308 : 1e308;
    }
    return sin(value) / c;
}

static double asin_reduced(double x)
{
    double square = x * x;

    return x * (1.0 +
           square * (1.0 / 6.0 +
           square * (3.0 / 40.0 +
           square * (15.0 / 336.0 +
           square * (105.0 / 3456.0)))));
}

double asin(double value)
{
    if (value > 1.0) {
        value = 1.0;
    }
    if (value < -1.0) {
        value = -1.0;
    }
    if (value > 0.5) {
        return MATH_HALF_PI - asin(sqrt(1.0 - value * value));
    }
    if (value < -0.5) {
        return -MATH_HALF_PI - asin(sqrt(1.0 - value * value));
    }
    return asin_reduced(value);
}

double acos(double value)
{
    return MATH_HALF_PI - asin(value);
}

double atan(double value)
{
    bool invert = false;
    double square;
    double result;

    if (value < 0.0) {
        value = -value;
        invert = true;
    }
    if (value > 1.0) {
        value = 1.0 / value;
        invert = !invert;
    }
    square = value * value;
    result = value * (1.0 +
              square * (-1.0 / 3.0 +
              square * (1.0 / 5.0 +
              square * (-1.0 / 7.0 +
              square * (1.0 / 9.0 +
              square * (-1.0 / 11.0))))));
    return invert ? (result < 0.0 ? -MATH_HALF_PI - result
                                  : MATH_HALF_PI - result)
                  : result;
}

double atan2(double y, double x)
{
    if (x > 0.0) {
        return atan(y / x);
    }
    if (x < 0.0) {
        return y >= 0.0 ? atan(y / x) + MATH_PI
                        : atan(y / x) - MATH_PI;
    }
    if (y > 0.0) {
        return MATH_HALF_PI;
    }
    if (y < 0.0) {
        return -MATH_HALF_PI;
    }
    return 0.0;
}

double exp(double value)
{
    int exponent = 0;
    double remainder;
    double term;
    double sum;
    int step;

    if (value < -709.0) {
        return 0.0;
    }
    if (value > 709.0) {
        return 1e308;
    }
    remainder = value;
    while (remainder > 0.5) {
        remainder -= 1.0;
        ++exponent;
    }
    while (remainder < -0.5) {
        remainder += 1.0;
        --exponent;
    }
    /* Taylor series for the remainder, which is now within [-0.5, 0.5] */
    term = 1.0;
    sum = 1.0;
    for (step = 1; step <= 18; ++step) {
        term *= remainder / (double)step;
        sum += term;
    }
    while (exponent > 0) {
        sum *= 2.0;
        --exponent;
    }
    while (exponent < 0) {
        sum *= 0.5;
        ++exponent;
    }
    return sum;
}

double log(double value)
{
    int exponent = 0;
    double mantissa;
    double square;
    double term;
    double sum;
    int step;

    if (value <= 0.0) {
        return value == 0.0 ? -1e308 : 0.0 / 1.0;
    }
    mantissa = value;
    while (mantissa >= 2.0) {
        mantissa *= 0.5;
        ++exponent;
    }
    while (mantissa < 1.0) {
        mantissa *= 2.0;
        --exponent;
    }
    /* atanh series: log(m) = 2 * (z + z^3/3 + ...) with z = (m-1)/(m+1) */
    {
        double z = (mantissa - 1.0) / (mantissa + 1.0);

        square = z * z;
        term = z;
        sum = z;
        for (step = 1; step <= 24; ++step) {
            term *= square;
            sum += term / (double)(2 * step + 1);
        }
        return 2.0 * sum + (double)exponent * 0.69314718055994530942;
    }
}

double log10(double value)
{
    return log(value) / 2.30258509299404568402;
}

double log2(double value)
{
    return log(value) / 0.69314718055994530942;
}

double pow(double base, double exponent)
{
    /* integral exponents go through repeated multiplication, which keeps
     * exact results such as pow(2,10) == 1024 */
    if (exponent == floor(exponent) && fabs(exponent) <= 1024.0) {
        long whole = (long)exponent;
        double result = 1.0;
        long step;

        if (whole < 0) {
            long inverse = -whole;

            for (step = 0; step < inverse; ++step) {
                result /= base;
            }
            return result;
        }
        for (step = 0; step < whole; ++step) {
            result *= base;
        }
        return result;
    }
    if (base < 0.0) {
        return 0.0 / 1.0;
    }
    if (base == 0.0) {
        return exponent == 0.0 ? 1.0 : 0.0;
    }
    return exp(exponent * log(base));
}

double fmod(double value, double divisor)
{
    double quotient;

    if (divisor == 0.0) {
        return 0.0 / 1.0;
    }
    quotient = value / divisor;
    quotient = quotient < 0.0 ? -floor(-quotient) : floor(quotient);
    return value - quotient * divisor;
}

double ldexp(double value, int exponent)
{
    double result = value;

    while (exponent > 0) {
        result *= 2.0;
        --exponent;
    }
    while (exponent < 0) {
        result *= 0.5;
        ++exponent;
    }
    return result;
}

double frexp(double value, int *out_exponent)
{
    int exponent = 0;
    double mantissa = value;

    if (mantissa == 0.0 || mantissa != mantissa) {
        *out_exponent = 0;
        return mantissa;
    }
    while (mantissa >= 1.0) {
        mantissa *= 0.5;
        ++exponent;
    }
    while (mantissa < 0.5) {
        mantissa *= 2.0;
        --exponent;
    }
    *out_exponent = exponent;
    return mantissa;
}

double modf(double value, double *out_integral)
{
    double integral = value < 0.0 ? ceil(value) : floor(value);

    *out_integral = integral;
    return value - integral;
}

double sinh(double value)
{
    return (exp(value) - exp(-value)) * 0.5;
}

double cosh(double value)
{
    return (exp(value) + exp(-value)) * 0.5;
}

double tanh(double value)
{
    double e = exp(2.0 * value);

    return (e - 1.0) / (e + 1.0);
}

/* --------------------------------------------------------------- time ---- */
/* Wall clock is meaningless in a kernel, so time() reports monotonic
 * milliseconds since boot.  Lua only uses it to seed math.random. */

extern uint64_t pit_ticks(void);

time_t time(time_t *out_value)
{
    uint64_t now = pit_ticks();

    if (out_value != 0) {
        *out_value = (time_t)(now / 1000U);
    }
    return (time_t)(now / 1000U);
}

double difftime(time_t end, time_t start)
{
    return (double)end - (double)start;
}

clock_t clock(void)
{
    return (clock_t)(pit_ticks() * 1000U / 1000000U);
}

/* -------------------------------------------------------- lua leftovers ---- */
/* Lua's locale handling and its bytecode reader/writer reference these even
 * when the io library and undump are never used, because the code is compiled
 * in.  A kernel has one locale, so the C locale is the only answer. */

struct lua_locale {
    char *decimal_point;
    char *thousands_sep;
    char *grouping;
};

static struct lua_locale c_locale = { (char *)".", (char *)"", (char *)"" };

struct lua_locale *localeconv(void)
{
    return &c_locale;
}

int strcoll(const char *left, const char *right)
{
    while (*left != 0 && *left == *right) {
        ++left;
        ++right;
    }
    return (int)(unsigned char)*left - (int)(unsigned char)*right;
}

size_t strxfrm(char *destination, const char *source, size_t count)
{
    size_t at = 0;

    while (source[at] != 0 && at + 1 < count) {
        destination[at] = source[at];
        ++at;
    }
    destination[at] = 0;
    return at;
}

/* Reading and writing precompiled chunks is not supported: there is no
 * filesystem loader to feed one in, and erroring is better than trusting a
 * deserialiser. */
/* Declared with void* so libc.c stays independent of the Lua headers.  Lua
 * declares them as int(lua_State *, const void *, size_t, int), which is
 * pointer-compatible, so the definitions match at link time. */

int luaU_dump(void *state, const void *buff, size_t size, int strip)
{
    (void)state;
    (void)buff;
    (void)size;
    (void)strip;
    return 1; /* an error: dumping is not supported */
}

int luaU_undump(void *state, const void *buff, size_t size, size_t *next)
{
    (void)state;
    (void)buff;
    (void)size;
    if (next != 0) {
        *next = 0;
    }
    return 1; /* an error: undumping is not supported */
}

/* liolib is not built, but lauxlib keeps a read_line helper that references
 * stdio.  Nothing calls it, so these only need to exist and refuse. */

int feof(void *stream)
{
    (void)stream;
    return 1;
}

size_t fread(void *destination, size_t size, size_t count, void *stream)
{
    (void)destination;
    (void)size;
    (void)count;
    (void)stream;
    return 0;
}

int ferror(void *stream)
{
    (void)stream;
    return 0;
}

/* ------------------------------------------------------------ stdio ---- */
/* lauxlib carries a line reader and a couple of error helpers that reference
 * stdio even though the io library is not built.  These satisfy the linker;
 * the ones that would need a real stream report failure instead of pretending
 * to work. */

void *stderr = 0;
void *stdout = 0;
void *stdin = 0;

int fprintf(void *stream, const char *format, ...)
{
    (void)stream;
    (void)format;
    return -1;
}

int vfprintf(void *stream, const char *format, va_list arguments)
{
    (void)stream;
    (void)format;
    (void)arguments;
    return -1;
}

int fputs(const char *text, void *stream)
{
    (void)text;
    (void)stream;
    return -1;
}

int fputc(int character, void *stream)
{
    (void)character;
    (void)stream;
    return -1;
}

int putchar(int character)
{
    (void)character;
    return -1;
}

int puts(const char *text)
{
    (void)text;
    return -1;
}

int fflush(void *stream)
{
    (void)stream;
    return 0;
}

int getc(void *stream)
{
    (void)stream;
    return -1;
}

int getchar(void)
{
    return -1;
}

int putc(int character, void *stream)
{
    (void)character;
    (void)stream;
    return -1;
}

size_t fwrite(const void *source, size_t size, size_t count, void *stream)
{
    (void)source;
    (void)size;
    (void)count;
    (void)stream;
    return 0;
}

void setbuf(void *stream, char *buffer)
{
    (void)stream;
    (void)buffer;
}

int setvbuf(void *stream, char *buffer, int mode, size_t size)
{
    (void)stream;
    (void)buffer;
    (void)mode;
    (void)size;
    return -1;
}

void perror(const char *text)
{
    (void)text;
}

/* Lua allocates through lua_newstate, but a few code paths call free() on a
 * pointer that came from malloc().  Route both at the heap so the two agree. */
void *malloc(size_t bytes)
{
    return heap_malloc(bytes);
}

void *calloc(size_t count, size_t size)
{
    return heap_calloc(count, size);
}

void *realloc(void *address, size_t bytes)
{
    return heap_realloc(address, bytes);
}

void free(void *address)
{
    heap_free(address);
}

/* errno and strerror: lauxlib reports load failures through them.  A kernel
 * has one global errno slot, which matches the single-task model. */
static int libc_errno;

int *__errno_location(void)
{
    return &libc_errno;
}

const char *strerror(int code)
{
    switch (code) {
    case 0:
        return "no error";
    case 1:
        return "operation not permitted";
    case 2:
        return "no such file or directory";
    case 5:
        return "input/output error";
    case 12:
        return "out of memory";
    case 22:
        return "invalid argument";
    default:
        return "unknown error";
    }
}

void *fopen(const char *path, const char *mode)
{
    (void)path;
    (void)mode;
    libc_errno = 2;
    return 0;
}

int fclose(void *stream)
{
    (void)stream;
    return 0;
}

/* --------------------------------------------------- ctype locale tables ---- */
/* glibc's <ctype.h> resolves isalpha and friends through locale tables rather
 * than functions.  Lua's libraries include the real header, so the symbols have
 * to exist even though this kernel is permanently in the C locale.
 *
 * The layout is glibc's: a 384 entry int32 table, __ctype_b.  The upper and
 * lower tables are only ever indexed for the ranges glibc supports, and are
 * sized the same way. */
#define CTYPE_TABLE_ENTRIES 384

static const unsigned short int libc_ctype_lower[CTYPE_TABLE_ENTRIES] = {
    /* 0x00 - 0x1f, control characters map to themselves */
    0x0000, 0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006, 0x0007,
    0x0008, 0x0009, 0x000a, 0x000b, 0x000c, 0x000d, 0x000e, 0x000f,
    0x0010, 0x0011, 0x0012, 0x0013, 0x0014, 0x0015, 0x0016, 0x0017,
    0x0018, 0x0019, 0x001a, 0x001b, 0x001c, 0x001d, 0x001e, 0x001f,
    /* 0x20 - 0x2f: punctuation, unchanged */
    0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027,
    0x0028, 0x0029, 0x002a, 0x002b, 0x002c, 0x002d, 0x002e, 0x002f,
    /* 0x30 - 0x39: digits, unchanged */
    0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039,
    /* 0x3a - 0x40: punctuation, unchanged */
    0x003a, 0x003b, 0x003c, 0x003d, 0x003e, 0x003f, 0x0040,
    /* 0x41 - 0x5a: uppercase folds to lowercase */
    0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067, 0x0068,
    0x0069, 0x006a, 0x006b, 0x006c, 0x006d, 0x006e, 0x006f, 0x0070,
    0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077, 0x0078,
    0x0079, 0x007a,
    /* 0x5b - 0x60: punctuation, unchanged */
    0x005b, 0x005c, 0x005d, 0x005e, 0x005f, 0x0060,
    /* 0x61 - 0x7a: lowercase, unchanged */
    0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067, 0x0068,
    0x0069, 0x006a, 0x006b, 0x006c, 0x006d, 0x006e, 0x006f, 0x0070,
    0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077, 0x0078,
    0x0079, 0x007a,
    /* 0x7b - 0x7f, then 0x80 - 0xff: unchanged, with Latin-1 folding left
     * as identity because the C locale does not define it */
    0x007b, 0x007c, 0x007d, 0x007e, 0x007f,
    0x0080, 0x0081, 0x0082, 0x0083, 0x0084, 0x0085, 0x0086, 0x0087,
    0x0088, 0x0089, 0x008a, 0x008b, 0x008c, 0x008d, 0x008e, 0x008f,
    0x0090, 0x0091, 0x0092, 0x0093, 0x0094, 0x0095, 0x0096, 0x0097,
    0x0098, 0x0099, 0x009a, 0x009b, 0x009c, 0x009d, 0x009e, 0x009f,
    0x00a0, 0x00a1, 0x00a2, 0x00a3, 0x00a4, 0x00a5, 0x00a6, 0x00a7,
    0x00a8, 0x00a9, 0x00aa, 0x00ab, 0x00ac, 0x00ad, 0x00ae, 0x00af,
    0x00b0, 0x00b1, 0x00b2, 0x00b3, 0x00b4, 0x00b5, 0x00b6, 0x00b7,
    0x00b8, 0x00b9, 0x00ba, 0x00bb, 0x00bc, 0x00bd, 0x00be, 0x00bf,
    0x00c0, 0x00c1, 0x00c2, 0x00c3, 0x00c4, 0x00c5, 0x00c6, 0x00c7,
    0x00c8, 0x00c9, 0x00ca, 0x00cb, 0x00cc, 0x00cd, 0x00ce, 0x00cf,
    0x00d0, 0x00d1, 0x00d2, 0x00d3, 0x00d4, 0x00d5, 0x00d6, 0x00d7,
    0x00d8, 0x00d9, 0x00da, 0x00db, 0x00dc, 0x00dd, 0x00de, 0x00df,
    0x00e0, 0x00e1, 0x00e2, 0x00e3, 0x00e4, 0x00e5, 0x00e6, 0x00e7,
    0x00e8, 0x00e9, 0x00ea, 0x00eb, 0x00ec, 0x00ed, 0x00ee, 0x00ef,
    0x00f0, 0x00f1, 0x00f2, 0x00f3, 0x00f4, 0x00f5, 0x00f6, 0x00f7,
    0x00f8, 0x00f9, 0x00fa, 0x00fb, 0x00fc, 0x00fd, 0x00fe, 0x00ff,
    0x0100, 0x0101, 0x0102, 0x0103, 0x0104, 0x0105, 0x0106, 0x0107,
    0x0108, 0x0109, 0x010a, 0x010b, 0x010c, 0x010d, 0x010e, 0x010f,
    0x0110, 0x0111, 0x0112, 0x0113, 0x0114, 0x0115, 0x0116, 0x0117,
    0x0118, 0x0119, 0x011a, 0x011b, 0x011c, 0x011d, 0x011e, 0x011f,
    0x0120, 0x0121, 0x0122, 0x0123, 0x0124, 0x0125, 0x0126, 0x0127,
    0x0128, 0x0129, 0x012a, 0x012b, 0x012c, 0x012d, 0x012e, 0x012f,
    0x0130, 0x0131, 0x0132, 0x0133, 0x0134, 0x0135, 0x0136, 0x0137,
    0x0138, 0x0139, 0x013a, 0x013b, 0x013c, 0x013d, 0x013e, 0x013f,
    0x0140, 0x0141, 0x0142, 0x0143, 0x0144, 0x0145, 0x0146, 0x0147,
    0x0148, 0x0149, 0x014a, 0x014b, 0x014c, 0x014d, 0x014e, 0x014f,
    0x0150, 0x0151, 0x0152, 0x0153, 0x0154, 0x0155, 0x0156, 0x0157,
    0x0158, 0x0159, 0x015a, 0x015b, 0x015c, 0x015d, 0x015e, 0x015f,
    0x0160, 0x0161, 0x0162, 0x0163, 0x0164, 0x0165, 0x0166, 0x0167,
    0x0168, 0x0169, 0x016a, 0x016b, 0x016c, 0x016d, 0x016e, 0x016f,
    0x0170, 0x0171, 0x0172, 0x0173, 0x0174, 0x0175, 0x0176, 0x0177,
    0x0178, 0x0179, 0x017a, 0x017b, 0x017c, 0x017d, 0x017e, 0x017f,
};

/* the class bits: _ISalnum _ISalpha _ISblank _IScntrl _ISdigit _ISgraph
 * _ISlower _ISprint _ISpunct _ISspace _ISupper _ISxdigit, in glibc's order */
static const int32_t libc_ctype_class[CTYPE_TABLE_ENTRIES] = {
    /* controls: cntrl */
    0x0004, 0x0004, 0x0004, 0x0004, 0x0004, 0x0004, 0x0004, 0x0004,
    0x0004, 0x0004, 0x0004, 0x0004, 0x0004, 0x0004, 0x0004, 0x0004,
    0x0004, 0x0004, 0x0004, 0x0004, 0x0004, 0x0004, 0x0004, 0x0004,
    0x0004, 0x0004, 0x0004, 0x0004, 0x0004, 0x0004, 0x0004, 0x0004,
    /* 0x20-0x23: graph + print */
    0x0100, 0x0100, 0x0100, 0x0100,
    /* 0x24-0x26 punct */
    0x0400, 0x0400, 0x0400,
    /* 0x27-0x29 punct */
    0x0400, 0x0400, 0x0400,
    /* 0x2a-0x2b punct */
    0x0400, 0x0400,
    /* 0x2c-0x2f punct */
    0x0400, 0x0400, 0x0400, 0x0400,
    /* digits: alnum digit xdigit */
    0x040a, 0x040a, 0x040a, 0x040a, 0x040a, 0x040a, 0x040a, 0x040a,
    0x040a, 0x040a,
    /* 0x3a-0x3b punct, 0x3c-0x3e graph, 0x3f print */
    0x0400, 0x0400, 0x0100, 0x0100, 0x0100, 0x0100,
    /* 0x40 punct */
    0x0400,
    /* A-Z: alnum alpha print upper xdigit */
    0x020a, 0x020a, 0x020a, 0x020a, 0x020a, 0x020a, 0x020a, 0x020a,
    0x020a, 0x020a, 0x020a, 0x020a, 0x020a, 0x020a, 0x020a, 0x020a,
    0x020a, 0x020a, 0x020a, 0x020a, 0x020a, 0x020a, 0x020a, 0x020a,
    0x020a, 0x020a,
    /* 0x5b-0x5e punct, 0x5f print, 0x60 punct */
    0x0400, 0x0400, 0x0400, 0x0400, 0x0100, 0x0400,
    /* a-z: alnum alpha lower print */
    0x020a, 0x020a, 0x020a, 0x020a, 0x020a, 0x020a, 0x020a, 0x020a,
    0x020a, 0x020a, 0x020a, 0x020a, 0x020a, 0x020a, 0x020a, 0x020a,
    0x020a, 0x020a, 0x020a, 0x020a, 0x020a, 0x020a, 0x020a, 0x020a,
    0x020a, 0x020a,
    /* 0x7b-0x7e punct, 0x7f cntrl */
    0x0400, 0x0400, 0x0400, 0x0400, 0x0004,
    /* 0x80-0xff are graph in the C locale */
    0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100,
    0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100,
    0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100,
    0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100,
    0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100,
    0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100,
    0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100,
    0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100,
    0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100,
    0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100,
    0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100,
    0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100,
    0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100,
};

const int32_t *__ctype_b_loc(void)
{
    return libc_ctype_class;
}

const int32_t *__ctype_tolower_loc(void)
{
    /* the lower table is indexed as unsigned short */
    return (const int32_t *)(const void *)libc_ctype_lower;
}

const int32_t *__ctype_toupper_loc(void)
{
    return (const int32_t *)(const void *)libc_ctype_lower;
}

void *fopen64(const char *path, const char *mode)
{
    return fopen(path, mode);
}

void *freopen64(const char *path, const char *mode, void *stream)
{
    (void)path;
    (void)mode;
    (void)stream;
    libc_errno = 2;
    return 0;
}
