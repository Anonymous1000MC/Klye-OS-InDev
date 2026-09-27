#ifndef KLYE_MMU_H
#define KLYE_MMU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Dynamic virtual memory.
 *
 * The boot code identity-maps RAM with 2 MiB pages, which is fine for the
 * kernel but means a large buffer has to be physically contiguous.  It often is
 * not: the Multiboot map hands back the usable regions around the kernel and
 * the framebuffer, so a 12 MiB run usually does not exist even when there is
 * hundreds of megabytes free.  That is what stopped a WAD-sized file from
 * loading.
 *
 * So a large buffer here is virtually contiguous and physically scattered, one
 * 4 KiB frame per page.  Fresh page tables are built in a spare 512 GB slot of
 * the top level table, found by looking for an unused entry rather than by
 * assuming one, so nothing already mapped is disturbed.
 */

#define VM_PAGE_BYTES 4096U

/* Prepare the mapping window.  Safe to call more than once. */
bool vm_init(void);

/* Reserve `bytes` of virtually contiguous, physically scattered memory.
 * Returns a virtual address that the caller may use directly, or 0 when the
 * frames or the tables are unavailable.  Zero filled. */
void *vm_alloc_pages(size_t bytes);

/* Release a reservation from vm_alloc_pages. */
void vm_free_pages(void *address, size_t bytes);

/* Map one 4 KiB physical page at a virtual address.  Both must be 4 KiB
 * aligned.  Used by vm_alloc_pages; exposed for callers driving the tables
 * themselves. */
bool vm_map_page(uint64_t virtual_address, uint64_t physical_address);

/* Drop the mapping at a 4 KiB aligned virtual address. */
void vm_unmap_page(uint64_t virtual_address);

/* Translate a virtual address to its physical address, or 0 when unmapped. */
uint64_t vm_to_physical(uint64_t virtual_address);

/* Virtual address a page-table entry holds, with the flag bits removed. */
uint64_t vm_entry_address(uint64_t entry);

/* Reason for the most recent failure, or "" if none. */
const char *vm_error(void);

#endif
