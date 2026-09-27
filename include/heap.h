#ifndef KLYE_HEAP_H
#define KLYE_HEAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Physical page frames plus a malloc front end, both built on the Multiboot
 * memory map.
 *
 * This is the "PMM-lite" half of the plan: enough to hand real pages to a
 * runtime such as Lua, without any page tables.  Extending it into a full PMM
 * later only means adding map/unmap on top of what is here.
 *
 * The two layers never overlap.  The bitmap owns every frame in the pool and
 * serves heap_alloc_pages(); malloc asks the bitmap for a contiguous run and
 * then sub-allocates from it with a free list, so the two can safely coexist.
 */

void heap_init(const void *map_base, uint32_t map_length,
               uint64_t kernel_high_water);

/* Raw frames.  `bytes` is rounded up to a 4 KiB page.  Returns 0 when no run
 * of that size is free. */
void *heap_alloc_pages(size_t bytes);
void heap_free_pages(void *address, size_t bytes);

/* malloc family.  heap_free() returns memory to the free list, which is what
 * Lua's collector needs: a bump allocator would leak an arena per cycle. */
void *heap_malloc(size_t bytes);
void *heap_calloc(size_t count, size_t size);
void *heap_realloc(void *address, size_t bytes);
void heap_free(void *address);

size_t heap_total_bytes(void);
size_t heap_free_bytes(void);
size_t heap_largest_block(void);
size_t heap_live_bytes(void);
uint32_t heap_page_count(void);
uint32_t heap_free_page_count(void);
bool heap_ready(void);

#endif
