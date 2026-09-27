/* heap.c - physical page frames plus a malloc front end.
 *
 * Layer 1: heap_alloc_pages / heap_free_pages carve 4 KiB frames out of one
 *          contiguous span of Multiboot-available memory, tracked in a bitmap.
 * Layer 2: heap_malloc and friends take contiguous runs from layer 1 and
 *          sub-allocate with a free list, so freed blocks are reusable.
 *
 * The pool deliberately starts above the kernel's own BSS and framebuffer
 * buffer, so nothing here can overlap the image.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "heap.h"

#define PAGE_BYTES 4096U
#define HEAP_MAGIC 0x4B4C59454652414DULL /* "KLYE_FRAM" */

/* How much of the pool malloc may hold at once.  It grows on demand, so this
 * is only the first reservation. */
#define HEAP_MALLOC_FIRST_RUN (2U * 1024U * 1024U)

/* Upper bound on trailing free pages folded into a run (256 KiB). */
#define HEAP_RUN_ABSORB_MAX 64U

/* Below this much RAM the pool stays disabled rather than starving a small
 * machine of the memory the kernel still needs. */
#define HEAP_MIN_RAM (48ULL * 1024ULL * 1024ULL)

struct free_block {
    struct free_block *next;
    size_t size;
};

struct alloc_header {
    uint64_t magic;
    size_t size;
};

_Static_assert(sizeof(struct alloc_header) == 16,
               "allocation header must stay 16 bytes");
_Static_assert(PAGE_BYTES % 16U == 0U, "page size must be header aligned");
_Static_assert(sizeof(struct free_block) <= 16U,
               "free block must fit in the header space");

static uint8_t *pool_base;
static size_t pool_bytes;
static uint32_t page_total;
static uint32_t page_cursor;
static uint32_t *page_bitmap;

static struct free_block *free_list;
static size_t live_bytes;

static inline bool bit_test(uint32_t index)
{
    return (page_bitmap[index >> 5] & (uint32_t)(1U << (index & 31U))) != 0U;
}

static inline void bit_set(uint32_t index)
{
    page_bitmap[index >> 5] |= (uint32_t)(1U << (index & 31U));
}

static inline void bit_clear(uint32_t index)
{
    page_bitmap[index >> 5] &= (uint32_t)~(1U << (index & 31U));
}

/* Counts free frames.  Only used for reporting, so a simple scan is fine. */
static uint32_t count_free_frames(void)
{
    uint32_t free_count = 0;

    for (uint32_t index = 0; index < page_total; ++index) {
        if (bit_test(index)) {
            ++free_count;
        }
    }
    return free_count;
}

/* Inserts keeping ascending address order so neighbours can merge later. */
static void free_list_insert(struct free_block *block)
{
    struct free_block **link = &free_list;

    while (*link != 0 && (uint8_t *)(*link) < (uint8_t *)block) {
        link = &(*link)->next;
    }
    block->next = *link;
    *link = block;
}

static void free_list_coalesce(void)
{
    struct free_block *block = free_list;

    while (block != 0 && block->next != 0) {
        if ((uint8_t *)block + block->size == (uint8_t *)block->next) {
            struct free_block *joined = block->next;

            block->size += joined->size;
            block->next = joined->next;
            continue;
        }
        block = block->next;
    }
}

/* Removes `bytes` from the free list, splitting when the block is large
 * enough to keep the tail. */
static void *free_list_take(size_t bytes)
{
    struct free_block **link = &free_list;

    while (*link != 0) {
        struct free_block *block = *link;

        if (block->size >= bytes) {
            if (block->size >= bytes + sizeof(struct free_block)) {
                struct free_block *rest =
                    (struct free_block *)(void *)((uint8_t *)(uintptr_t)block +
                                                  bytes);

                rest->size = block->size - bytes;
                rest->next = block->next;
                block->size = bytes;
                block->next = 0;
                *link = rest;
            } else {
                *link = block->next;
                block->next = 0;
            }
            return block;
        }
        link = &block->next;
    }
    return 0;
}

/* Marks `wanted` contiguous free frames as used and returns the run.  When
 * `out_bytes` is non-null it receives the exact size of the run taken, which
 * may be larger than asked for so neighbouring free pages are not stranded. */
static void *take_run(uint32_t wanted, size_t *out_bytes)
{
    uint32_t run = 0;
    uint32_t start = 0;
    uint32_t extra = 0;

    if (page_bitmap == 0 || wanted == 0U || wanted > page_total) {
        return 0;
    }
    for (uint32_t scanned = 0; scanned < page_total; ++scanned) {
        uint32_t index = (page_cursor + scanned) % page_total;

        if (bit_test(index)) {
            if (run == 0U) {
                start = index;
            }
            ++run;
            if (run >= wanted) {
                break;
            }
        } else {
            run = 0;
        }
    }
    if (run < wanted) {
        return 0;
    }
    /* Absorb a few trailing free pages so a slightly larger request can still
     * be met without leaving unusable one page gaps behind.  Bounded on
     * purpose: absorbing the whole pool would starve heap_alloc_pages(). */
    while (extra < HEAP_RUN_ABSORB_MAX && start + run + extra < page_total &&
           bit_test((start + run + extra) % page_total) != 0U) {
        ++extra;
    }
    for (uint32_t at = 0; at < run + extra; ++at) {
        bit_clear((start + at) % page_total);
    }
    page_cursor = (start + run + extra) % page_total;
    if (out_bytes != 0) {
        *out_bytes = (size_t)(run + extra) * PAGE_BYTES;
    }
    return pool_base + (size_t)start * PAGE_BYTES;
}

/* Asks the bitmap for another run and donates it to the free list. */
static bool free_list_grow(size_t at_least)
{
    size_t want = at_least;
    size_t got = 0;
    void *run;

    if (want < HEAP_MALLOC_FIRST_RUN) {
        want = HEAP_MALLOC_FIRST_RUN;
    }
    if (want > HEAP_MALLOC_FIRST_RUN * 4U) {
        want = HEAP_MALLOC_FIRST_RUN * 4U;
    }
    run = take_run((uint32_t)((want + PAGE_BYTES - 1U) / PAGE_BYTES), &got);
    if (run == 0) {
        /* Even one page is better than failing outright. */
        run = take_run(1U, &got);
        if (run == 0) {
            return false;
        }
    }
    {
        struct free_block *block = (struct free_block *)(uintptr_t)run;

        block->size = got;
        block->next = 0;
        free_list_insert(block);
        free_list_coalesce();
    }
    return true;
}

void heap_init(const void *map_base, uint32_t map_length,
               uint64_t kernel_high_water)
{
    const uint8_t *cursor;
    uint64_t best_base = 0;
    uint64_t best_length = 0;
    uint64_t lowest_available = 0;
    bool seen_any = false;
    size_t bitmap_bytes;
    uint32_t frames;

    pool_base = 0;
    pool_bytes = 0;
    page_total = 0;
    page_cursor = 0;
    free_list = 0;
    live_bytes = 0;
    page_bitmap = 0;

    if (map_base == 0 || map_length < 24U) {
        return;
    }
    /* Pick the largest available run that lies entirely above the kernel. */
    cursor = (const uint8_t *)map_base;
    while (cursor + 24U <= (const uint8_t *)map_base + map_length) {
        uint64_t addr;
        uint64_t length;
        uint32_t kind;

        __builtin_memcpy(&addr, cursor, sizeof(addr));
        __builtin_memcpy(&length, cursor + 8, sizeof(length));
        __builtin_memcpy(&kind, cursor + 16, sizeof(kind));
        if (kind == 1U && length != 0U) {
            uint64_t start = addr;
            uint64_t end = addr + length;

            if (start < 0x100000ULL) {
                start = 0x100000ULL;
            }
            if (end > start) {
                if (!seen_any || start < lowest_available) {
                    lowest_available = start;
                    seen_any = true;
                }
                /* The kernel usually sits inside one large available region,
                 * so the usable part is whatever is above the image rather
                 * than the whole region. */
                {
                    uint64_t usable = start;

                    if (usable < kernel_high_water) {
                        usable = kernel_high_water;
                    }
                    if (end > usable && end - usable > best_length) {
                        best_base = usable;
                        best_length = end - usable;
                    }
                }
            }
        }
        cursor += 24U;
    }
    if (best_base == 0U) {
        return;
    }
    best_base = (best_base + PAGE_BYTES - 1U) &
                ~(uint64_t)(PAGE_BYTES - 1U);
    best_length &= ~(uint64_t)(PAGE_BYTES - 1U);
    if (best_length < PAGE_BYTES * 64U) {
        return;
    }
    if (lowest_available != 0U &&
        best_length + lowest_available < HEAP_MIN_RAM) {
        return;
    }
    frames = (uint32_t)(best_length / PAGE_BYTES);
    if (frames > 262144U) {
        frames = 262144U; /* 1 GiB of frames; the bitmap is 32 KiB */
    }
    bitmap_bytes = ((size_t)frames + 31U) / 32U * sizeof(uint32_t);

    /* The bitmap cannot live in a frame it tracks, so carve it from the base
     * of the pool and shrink the pool to match. */
    if (bitmap_bytes + PAGE_BYTES * 16U >= (size_t)frames * PAGE_BYTES) {
        return;
    }
    page_bitmap = (uint32_t *)(uintptr_t)best_base;
    __builtin_memset(page_bitmap, 0, bitmap_bytes);
    {
        size_t skipped = (bitmap_bytes + PAGE_BYTES - 1U) & ~(size_t)(PAGE_BYTES - 1U);
        uint32_t skip_frames = (uint32_t)(skipped / PAGE_BYTES);

        pool_base = (uint8_t *)(uintptr_t)best_base + skipped;
        pool_bytes = (size_t)(frames - skip_frames) * PAGE_BYTES;
        page_total = frames - skip_frames;
    }
    for (uint32_t index = 0; index < page_total; ++index) {
        bit_set(index);
    }
    /* Prime malloc so the first allocation does not have to grow. */
    free_list_grow(HEAP_MALLOC_FIRST_RUN);
}

void *heap_alloc_pages(size_t bytes)
{
    if (page_bitmap == 0 || bytes == 0U) {
        return 0;
    }
    return take_run((uint32_t)((bytes + PAGE_BYTES - 1U) / PAGE_BYTES), 0);
}

void heap_free_pages(void *address, size_t bytes)
{
    uint32_t wanted;
    uintptr_t offset;
    uint32_t first;

    if (page_bitmap == 0 || address == 0 || bytes == 0U) {
        return;
    }
    wanted = (uint32_t)((bytes + PAGE_BYTES - 1U) / PAGE_BYTES);
    offset = (uintptr_t)address - (uintptr_t)pool_base;
    if (offset % PAGE_BYTES != 0U) {
        return;
    }
    first = (uint32_t)(offset / PAGE_BYTES);
    if (first >= page_total) {
        return;
    }
    for (uint32_t at = 0; at < wanted && first + at < page_total; ++at) {
        bit_set(first + at);
    }
}

void *heap_malloc(size_t bytes)
{
    struct alloc_header *header;
    size_t total;

    if (bytes == 0U) {
        return 0;
    }
    total = bytes + sizeof(struct alloc_header);
    header = (struct alloc_header *)(uintptr_t)free_list_take(total);
    if (header == 0) {
        /* Try to satisfy from the page pool, then retry. */
        if (free_list_grow(total) == false) {
            return 0;
        }
        header = (struct alloc_header *)(uintptr_t)free_list_take(total);
        if (header == 0) {
            return 0;
        }
    }
    header->magic = HEAP_MAGIC;
    header->size = total;
    live_bytes += total;
    return (void *)((uint8_t *)(uintptr_t)header + sizeof(*header));
}

void *heap_calloc(size_t count, size_t size)
{
    size_t total;
    void *block;

    if (count != 0U && size > (size_t)-1 / count) {
        return 0;
    }
    total = count * size;
    block = heap_malloc(total);
    if (block != 0) {
        __builtin_memset(block, 0, total);
    }
    return block;
}

void *heap_realloc(void *address, size_t bytes)
{
    struct alloc_header *header;
    void *block;
    size_t old_size;

    if (address == 0) {
        return heap_malloc(bytes);
    }
    if (bytes == 0U) {
        heap_free(address);
        return 0;
    }
    header = (struct alloc_header *)(void *)((uint8_t *)(uintptr_t)address -
                                             sizeof(*header));
    if (header->magic != HEAP_MAGIC) {
        return 0;
    }
    old_size = header->size - sizeof(*header);
    if (old_size >= bytes) {
        return address;
    }
    block = heap_malloc(bytes);
    if (block == 0) {
        return 0;
    }
    __builtin_memcpy(block, address, old_size);
    heap_free(address);
    return block;
}

void heap_free(void *address)
{
    struct alloc_header *header;

    if (address == 0) {
        return;
    }
    header = (struct alloc_header *)(void *)((uint8_t *)(uintptr_t)address -
                                             sizeof(*header));
    if (header->magic != HEAP_MAGIC) {
        return;
    }
    if (live_bytes >= header->size) {
        live_bytes -= header->size;
    }
    header->magic = 0U;
    /* The header doubles as a free_block: same size, and the magic is clear. */
    free_list_insert((struct free_block *)(uintptr_t)header);
    free_list_coalesce();
}

size_t heap_total_bytes(void)
{
    return pool_bytes;
}

size_t heap_free_bytes(void)
{
    struct free_block *block = free_list;
    size_t total = 0;

    while (block != 0) {
        total += block->size;
        block = block->next;
    }
    return total;
}

size_t heap_largest_block(void)
{
    struct free_block *block = free_list;
    size_t best = 0;

    while (block != 0) {
        if (block->size > best) {
            best = block->size;
        }
        block = block->next;
    }
    return best;
}

size_t heap_live_bytes(void)
{
    return live_bytes;
}

uint32_t heap_page_count(void)
{
    return page_total;
}

uint32_t heap_free_page_count(void)
{
    return count_free_frames();
}

bool heap_ready(void)
{
    return page_bitmap != 0 && page_total != 0U;
}
