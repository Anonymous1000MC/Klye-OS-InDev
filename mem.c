#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void *memcpy(void *destination, const void *source, size_t count)
{
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;
    uint64_t *wide_out = (uint64_t *)(void *)out;
    const uint64_t *wide_in = (const uint64_t *)(const void *)in;
    size_t words = count / 8U;

    for (size_t index = 0; index < words; ++index) {
        wide_out[index] = wide_in[index];
    }
    for (size_t index = words * 8U; index < count; ++index) {
        out[index] = in[index];
    }
    return destination;
}

void *memmove(void *destination, const void *source, size_t count)
{
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;

    if (out == in || count == 0U) {
        return destination;
    }
    if (out < in) {
        for (size_t index = 0; index < count; ++index) {
            out[index] = in[index];
        }
        return destination;
    }
    for (size_t index = count; index > 0U; --index) {
        out[index - 1U] = in[index - 1U];
    }
    return destination;
}

void *memset(void *destination, int value, size_t count)
{
    uint8_t *out = (uint8_t *)destination;
    uint64_t pattern = (uint64_t)(uint8_t)value;
    uint64_t *wide = (uint64_t *)(void *)out;
    size_t words = count / 8U;

    pattern |= pattern << 8;
    pattern |= pattern << 16;
    pattern |= pattern << 32;
    for (size_t index = 0; index < words; ++index) {
        wide[index] = pattern;
    }
    for (size_t index = words * 8U; index < count; ++index) {
        out[index] = (uint8_t)value;
    }
    return destination;
}

int memcmp(const void *left, const void *right, size_t count)
{
    const uint8_t *a = (const uint8_t *)left;
    const uint8_t *b = (const uint8_t *)right;

    for (size_t index = 0; index < count; ++index) {
        if (a[index] != b[index]) {
            return (int)a[index] - (int)b[index];
        }
    }
    return 0;
}
