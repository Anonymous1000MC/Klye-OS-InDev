/* mmu.c - page-table walking and dynamically mapped memory.  See include/mmu.h
 * for why this exists. */

#include "mmu.h"
#include "heap.h"

#define PTE_PRESENT   0x001U
#define PTE_WRITE     0x002U
#define PTE_USER      0x004U
#define PTE_PWT       0x008U
#define PTE_PCD       0x010U
#define PTE_ACCESSED  0x020U
#define PTE_DIRTY     0x040U
#define PTE_LARGE     0x080U
#define PTE_GLOBAL    0x100U

#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

/* Everything is mapped present and writable, and deliberately not NX, so no
 * separate executable mapping is needed and the code can live in a mapped
 * buffer.  The kernel has no need to keep text read-only right now. */
#define PTE_FLAGS     (PTE_PRESENT | PTE_WRITE)

#define VM_WINDOW_SIZE   (512ULL * 1024ULL * 1024ULL * 1024ULL) /* one PML4 slot */
#define VM_MAX_RESERVE   64U

static uint64_t vm_window_base;
static uint64_t vm_window_next;
static uint64_t vm_window_limit;
static bool vm_window_ready;
static char vm_error_text[64] = "";

static void vm_fail(const char *why)
{
    __builtin_strncpy(vm_error_text, why, sizeof(vm_error_text) - 1);
    vm_error_text[sizeof(vm_error_text) - 1] = 0;
}

static uint64_t vm_read_cr3(void)
{
    uint64_t value;

    __asm__ volatile("mov %%cr3, %0" : "=r"(value));
    return value & ~0xFFFULL;
}

static void vm_write_cr3(uint64_t value)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(value) : "memory");
}

/* Flush the TLB for whatever CR3 currently points at.  The reload form is the
 * one that is actually guaranteed to flush: invlpg only covers one entry, and
 * after a page-table walk the old entries for this address could still be
 * cached. */
static void vm_flush_tlb(void)
{
    vm_write_cr3(vm_read_cr3());
}

uint64_t vm_entry_address(uint64_t entry)
{
    return entry & PTE_ADDR_MASK;
}

static uint64_t *vm_table_at(uint64_t physical)
{
    return (uint64_t *)(uintptr_t)physical;
}

static uint64_t *vm_next_table(uint64_t *table, uint32_t index, uint32_t flags)
{
    uint64_t entry = table[index];

    if ((entry & PTE_PRESENT) != 0U) {
        uint64_t address = vm_entry_address(entry);

        /* A large page cannot be split by writing a 4 KiB entry over it, so
         * this is reported rather than silently corrupting the mapping. */
        if ((entry & PTE_LARGE) != 0U) {
            vm_fail("a large page covers this address");
            return 0;
        }
        return vm_table_at(address);
    }
    {
        void *fresh = heap_alloc_frame();

        if (fresh == 0) {
            vm_fail("out of memory for a page table");
            return 0;
        }
        /* A fresh table has to be zeroed, and heap_alloc_frame can hand back a
         * frame malloc previously used, so do not assume it is clean. */
        {
            uint64_t *words = (uint64_t *)fresh;

            for (uint32_t index = 0; index < VM_PAGE_BYTES / 8U; ++index) {
                words[index] = 0;
            }
        }
        table[index] = (uint64_t)(uintptr_t)fresh | flags;
        return (uint64_t *)fresh;
    }
}

/* Walk to the leaf table for `virtual_address`, creating levels as needed.
 * Intermediate levels always get 4 KiB granularity, so this returns a page
 * table whose entries are leaf entries. */
static uint64_t *vm_leaf_table(uint64_t virtual_address)
{
    uint32_t pml4_index = (uint32_t)((virtual_address >> 39) & 0x1FFU);
    uint32_t pdpt_index = (uint32_t)((virtual_address >> 30) & 0x1FFU);
    uint32_t pd_index = (uint32_t)((virtual_address >> 21) & 0x1FFU);
    uint32_t pt_index = (uint32_t)((virtual_address >> 12) & 0x1FFU);
    uint64_t root = vm_read_cr3();
    uint64_t *level4 = vm_next_table(vm_table_at(root), pml4_index, PTE_FLAGS);
    uint64_t *level3;
    uint64_t *level2;
    uint64_t *level1;

    if (level4 == 0) {
        return 0;
    }
    level3 = vm_next_table(level4, pdpt_index, PTE_FLAGS);
    if (level3 == 0) {
        return 0;
    }
    level2 = vm_next_table(level3, pd_index, PTE_FLAGS);
    if (level2 == 0) {
        return 0;
    }
    level1 = vm_next_table(level2, pt_index, PTE_FLAGS);
    if (level1 == 0) {
        return 0;
    }
    return level1;
}

bool vm_init(void)
{
    uint64_t root;
    uint64_t *table;
    int chosen = -1;

    vm_error_text[0] = 0;
    if (vm_window_ready) {
        return true;
    }
    root = vm_read_cr3();
    table = vm_table_at(root);

    /* Find a top level slot nothing is using.  Entry 0 is the identity map, so
     * start past it rather than assuming a particular layout. */
    for (uint32_t index = 1; index < 512U; ++index) {
        if ((table[index] & PTE_PRESENT) == 0U) {
            chosen = (int)index;
            break;
        }
    }
    if (chosen < 0) {
        vm_fail("no free top level paging slot");
        return false;
    }
    vm_window_base = (uint64_t)chosen * VM_WINDOW_SIZE;
    vm_window_next = vm_window_base;
    vm_window_limit = vm_window_base + VM_WINDOW_SIZE;
    vm_window_ready = true;
    return true;
}

bool vm_map_page(uint64_t virtual_address, uint64_t physical_address)
{
    uint64_t *table;
    uint32_t index;

    if ((virtual_address % VM_PAGE_BYTES) != 0U ||
        (physical_address % VM_PAGE_BYTES) != 0U) {
        vm_fail("addresses must be page aligned");
        return false;
    }
    table = vm_leaf_table(virtual_address);
    if (table == 0) {
        return false;
    }
    index = (uint32_t)((virtual_address >> 12) & 0x1FFU);
    table[index] = (physical_address & PTE_ADDR_MASK) | PTE_FLAGS;
    vm_flush_tlb();
    return true;
}

void vm_unmap_page(uint64_t virtual_address)
{
    uint64_t *table;
    uint32_t index;

    if ((virtual_address % VM_PAGE_BYTES) != 0U) {
        return;
    }
    /* Only walk for the leaf here: unmapping must not create tables. */
    {
        uint32_t pml4_index = (uint32_t)((virtual_address >> 39) & 0x1FFU);
        uint32_t pdpt_index = (uint32_t)((virtual_address >> 30) & 0x1FFU);
        uint32_t pd_index = (uint32_t)((virtual_address >> 21) & 0x1FFU);
        uint64_t *level4 = vm_table_at(vm_read_cr3());
        uint64_t *level3;
        uint64_t *level2;

        if ((level4[pml4_index] & PTE_PRESENT) == 0U) {
            return;
        }
        level3 = vm_table_at(vm_entry_address(level4[pml4_index]));
        if ((level3[pdpt_index] & PTE_PRESENT) == 0U) {
            return;
        }
        level2 = vm_table_at(vm_entry_address(level3[pdpt_index]));
        if ((level2[pd_index] & PTE_PRESENT) == 0U) {
            return;
        }
        table = vm_table_at(vm_entry_address(level2[pd_index]));
        if ((table[pd_index] & PTE_LARGE) != 0U) {
            return;
        }
    }
    index = (uint32_t)((virtual_address >> 12) & 0x1FFU);
    if ((table[index] & PTE_PRESENT) == 0U) {
        return;
    }
    table[index] = 0;
    vm_flush_tlb();
}

void *vm_alloc_pages(size_t bytes)
{
    uint64_t start;
    uint32_t count;

    vm_error_text[0] = 0;
    if (bytes == 0U) {
        return 0;
    }
    if (!vm_init()) {
        return 0;
    }
    count = (uint32_t)((bytes + VM_PAGE_BYTES - 1U) / VM_PAGE_BYTES);
    if (vm_window_next + (uint64_t)count * VM_PAGE_BYTES > vm_window_limit) {
        vm_fail("the mapping window is full");
        return 0;
    }
    start = vm_window_next;
    for (uint32_t index = 0; index < count; ++index) {
        uint64_t virtual_address = start + (uint64_t)index * VM_PAGE_BYTES;
        void *frame = heap_alloc_frame();

        if (frame == 0) {
            /* give back what this call already took, so a failure does not
             * slowly drain the frame pool */
            for (uint32_t undo = 0; undo < index; ++undo) {
                uint64_t done = start + (uint64_t)undo * VM_PAGE_BYTES;
                uint64_t physical = vm_to_physical(done);

                vm_unmap_page(done);
                if (physical != 0U) {
                    heap_free_frame((void *)(uintptr_t)physical);
                }
            }
            vm_fail("out of memory for the pages");
            return 0;
        }
        {
            /* heap_alloc_frame can return a frame malloc has used before */
            uint64_t *words = (uint64_t *)frame;

            for (uint32_t at = 0; at < VM_PAGE_BYTES / 8U; ++at) {
                words[at] = 0;
            }
        }
        if (!vm_map_page(virtual_address, (uint64_t)(uintptr_t)frame)) {
            heap_free_frame(frame);
            return 0;
        }
    }
    vm_window_next += (uint64_t)count * VM_PAGE_BYTES;
    return (void *)(uintptr_t)start;
}

void vm_free_pages(void *address, size_t bytes)
{
    uint64_t start = (uint64_t)(uintptr_t)address;
    uint32_t count;

    if (address == 0 || bytes == 0U) {
        return;
    }
    count = (uint32_t)((bytes + VM_PAGE_BYTES - 1U) / VM_PAGE_BYTES);
    for (uint32_t index = 0; index < count; ++index) {
        uint64_t virtual_address = start + (uint64_t)index * VM_PAGE_BYTES;
        uint64_t physical = vm_to_physical(virtual_address);

        if (physical != 0U) {
            vm_unmap_page(virtual_address);
            heap_free_frame((void *)(uintptr_t)physical);
        }
    }
    /* give the window back so the space can be reused */
    if (vm_window_ready && start == vm_window_base) {
        vm_window_next = start;
    } else if (vm_window_ready && start < vm_window_next &&
               start + (uint64_t)count * VM_PAGE_BYTES == vm_window_next) {
        vm_window_next = start;
    }
}

uint64_t vm_to_physical(uint64_t virtual_address)
{
    uint32_t pml4_index = (uint32_t)((virtual_address >> 39) & 0x1FFU);
    uint32_t pdpt_index = (uint32_t)((virtual_address >> 30) & 0x1FFU);
    uint32_t pd_index = (uint32_t)((virtual_address >> 21) & 0x1FFU);
    uint32_t pt_index = (uint32_t)((virtual_address >> 12) & 0x1FFU);
    uint64_t *level4 = vm_table_at(vm_read_cr3());
    uint64_t *level3;
    uint64_t *level2;
    uint64_t *level1;
    uint64_t entry;

    if ((level4[pml4_index] & PTE_PRESENT) == 0U) {
        return 0;
    }
    level3 = vm_table_at(vm_entry_address(level4[pml4_index]));
    if ((level3[pdpt_index] & PTE_PRESENT) == 0U) {
        return 0;
    }
    level2 = vm_table_at(vm_entry_address(level3[pdpt_index]));
    if ((level2[pd_index] & PTE_PRESENT) == 0U) {
        return 0;
    }
    if ((level2[pd_index] & PTE_LARGE) != 0U) {
        /* a 2 MiB page covers the address whole */
        return vm_entry_address(level2[pd_index]) +
               (virtual_address & (2ULL * 1024ULL * 1024ULL - 1U));
    }
    level1 = vm_table_at(vm_entry_address(level2[pd_index]));
    if ((level1[pt_index] & PTE_PRESENT) == 0U) {
        return 0;
    }
    entry = level1[pt_index];
    if ((entry & PTE_LARGE) != 0U) {
        return vm_entry_address(entry) + (virtual_address & 0x1FFFFFULL);
    }
    return vm_entry_address(entry) + (virtual_address & (VM_PAGE_BYTES - 1U));
}

const char *vm_error(void)
{
    return vm_error_text;
}
