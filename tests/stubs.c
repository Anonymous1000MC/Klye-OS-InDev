/* Minimal stand-ins for the kernel symbols libc.c reaches for, so the host
 * tests in this directory can link against the real libc.c.  The tests only
 * exercise formatting, parsing, and math, so the allocator just needs to
 * behave and the timer just needs to exist. */

#include <stddef.h>
#include <stdint.h>

volatile uint64_t pit_ticks;

void *heap_malloc(size_t size)
{
    static char pool[1 << 20];
    static size_t used;

    if (size == 0) {
        size = 1;
    }
    used = (used + 15u) & ~(size_t)15u;
    if (used + size > sizeof(pool)) {
        return NULL;
    }
    void *result = &pool[used];

    used += size;
    return result;
}

void *heap_calloc(size_t count, size_t size)
{
    size_t total = count * size;
    unsigned char *out = (unsigned char *)heap_malloc(total);

    if (out != NULL) {
        for (size_t i = 0; i < total; ++i) {
            out[i] = 0;
        }
    }
    return out;
}

void *heap_realloc(void *ptr, size_t size)
{
    void *out = heap_malloc(size);

    if (out != NULL && ptr != NULL) {
        unsigned char *from = (unsigned char *)ptr;
        unsigned char *to = (unsigned char *)out;

        for (size_t i = 0; i < size; ++i) {
            to[i] = from[i];
        }
    }
    return out;
}

void heap_free(void *ptr)
{
    (void)ptr;
}
