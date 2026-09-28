/* Minimal stand-ins for the kernel symbols libc.c reaches for, so the host
 * tests in this directory can link against the real libc.c.  The tests cover
 * formatting, parsing, math and the filesystem, so the allocator has to behave
 * like one that can fail and the timer just needs to exist.
 *
 * The pool is big enough for the filesystem's tables, because the VFS test
 * mounts a real one rather than a mock. */

#include <stddef.h>
#include <stdint.h>

volatile uint64_t pit_ticks;

#define STUB_POOL_BYTES (16u << 20)

static char stub_pool[STUB_POOL_BYTES];
static size_t stub_used;

void *heap_malloc(size_t size)
{
    if (size == 0) {
        size = 1;
    }
    stub_used = (stub_used + 15u) & ~(size_t)15u;
    if (stub_used + size > sizeof(stub_pool)) {
        return NULL;
    }
    void *result = &stub_pool[stub_used];

    stub_used += size;
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

/* The filesystem prefers the MMU for its block store, because a heap run has
 * to be physically contiguous and several megabytes rarely is.  Here there is
 * no MMU, so this reports failure and the filesystem takes the heap path,
 * which is the branch a small guest without a mapping window would take. */
void *vm_alloc_pages(size_t bytes)
{
    (void)bytes;
    return NULL;
}

void vm_free_pages(void *address, size_t bytes)
{
    (void)address;
    (void)bytes;
}

/* The filesystem asks this before allocating its tables, so the stand-in has
 * to answer with what is genuinely left rather than reporting an endless pool
 * and letting the caller walk into a NULL it believed it had ruled out. */
size_t heap_largest_block(void)
{
    return sizeof(stub_pool) - stub_used;
}
