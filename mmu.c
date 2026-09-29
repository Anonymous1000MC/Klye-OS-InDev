/* mmu.c - page-table walking and dynamically mapped memory.  See include/mmu.h
 * for why this exists. */

#include "mmu.h"
#include "heap.h"
#include "kernel.h"

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

/* The same, but reachable from ring 3.
 *
 * The user bit is not something that can be set on the leaf entry alone.  The
 * processor checks it at every level of the walk, so a leaf marked user under a
 * supervisor-only directory is refused exactly as before, with a protection
 * violation rather than a missing page.  It is also not something that can be
 * set on the entries of an existing range: the program tables here share their
 * top level, second level and third level entries with the kernel, and marking
 * those user would hand ring 3 the whole gigabyte they cover, kernel included.
 *
 * So user memory gets its own top level slot, and every table in the walk down
 * from it is created with the user bit already set.  That is what
 * vm_user_alloc_pages is for. */
#define PTE_USER_FLAGS (PTE_PRESENT | PTE_WRITE | PTE_USER)

#define VM_WINDOW_SIZE   (512ULL * 1024ULL * 1024ULL * 1024ULL) /* one PML4 slot */
#define VM_MAX_RESERVE   64U

static uint64_t vm_window_base;
static uint64_t vm_window_next;
static uint64_t vm_window_limit;
static bool vm_window_ready;

/* The user window: same idea as the one above, but every entry in the walk
 * down to a leaf carries the user bit, so ring 3 can reach it.  It lives in its
 * own top level slot, which is the only way to get that without exposing
 * whatever else shares the intermediate tables. */
static uint64_t vm_user_base;
static uint64_t vm_user_next;
static uint64_t vm_user_limit;
static bool vm_user_ready;
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
/* The leaf page table: the table whose entries are page frames, so that
 * vm_map_page can index it by the low bits of a virtual address.
 *
 * Three levels of descent from the top: the top level entry, then the page
 * directory pointer, then the page table.  A fourth step would follow the page
 * table's entry and land on the page itself, which is not a table at all, and
 * a write there lands in the page's contents while the entry it was meant to
 * describe stays whatever it was.  Nothing fails: the store succeeds, reading
 * it back succeeds, and the address is simply never mapped.  The mapping
 * window is created by the same walk, so it inherits the same off by one and
 * the window's own entries are wrong too. */
static uint64_t *vm_leaf_table_flags(uint64_t virtual_address, uint32_t flags)
{
    uint32_t pml4_index = (uint32_t)((virtual_address >> 39) & 0x1FFU);
    uint32_t pdpt_index = (uint32_t)((virtual_address >> 30) & 0x1FFU);
    uint32_t pd_index = (uint32_t)((virtual_address >> 21) & 0x1FFU);
    uint64_t root = vm_read_cr3();
    uint64_t *pdpt = vm_next_table(vm_table_at(root), pml4_index, flags);
    uint64_t *pd;
    uint64_t *pt;

    if (pdpt == 0) {
        return 0;
    }
    pd = vm_next_table(pdpt, pdpt_index, flags);
    if (pd == 0) {
        return 0;
    }
    pt = vm_next_table(pd, pd_index, flags);
    if (pt == 0) {
        return 0;
    }
    return pt;
}

/* Claim a top level slot nothing is using, past the identity map.  Entry 0
 * holds the identity map, so start at 1 and search rather than assuming a
 * particular layout. */
static int vm_claim_slot(void)
{
    uint64_t root = vm_read_cr3();
    uint64_t *table = vm_table_at(root);

    for (uint32_t index = 1; index < 512U; ++index) {
        if ((table[index] & PTE_PRESENT) == 0U) {
            return (int)index;
        }
    }
    return -1;
}

bool vm_init(void)
{
    int chosen;

    vm_error_text[0] = 0;
    if (vm_window_ready) {
        return true;
    }
    chosen = vm_claim_slot();
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

static bool vm_user_init(void)
{
    int chosen;

    if (vm_user_ready) {
        return true;
    }
    /* Its own slot, and not a second range inside the kernel window: the
     * tables above the leaves have to carry the user bit too, and the ones
     * over the kernel window are shared with kernel mappings. */
    chosen = vm_claim_slot();
    if (chosen < 0) {
        vm_fail("no free top level paging slot for user memory");
        return false;
    }
    vm_user_base = (uint64_t)chosen * VM_WINDOW_SIZE;
    vm_user_next = vm_user_base;
    vm_user_limit = vm_user_base + VM_WINDOW_SIZE;
    vm_user_ready = true;
    return true;
}

static bool vm_map_page_flags(uint64_t virtual_address, uint64_t physical_address,
                              uint32_t flags)
{
    uint64_t *table;
    uint32_t index;

    if ((virtual_address % VM_PAGE_BYTES) != 0U ||
        (physical_address % VM_PAGE_BYTES) != 0U) {
        vm_fail("addresses must be page aligned");
        return false;
    }
    table = vm_leaf_table_flags(virtual_address, flags);
    if (table == 0) {
        return false;
    }
    index = (uint32_t)((virtual_address >> 12) & 0x1FFU);
    table[index] = (physical_address & PTE_ADDR_MASK) | flags;
    /* Read the entry back through the page tables rather than trusting the
     * store.  The leaf table is ordinary memory, and anything that maps it
     * wrongly writes somewhere else, which looks exactly like a working
     * mapping right up until something is read through it. */
    if ((table[index] & PTE_ADDR_MASK) != (physical_address & PTE_ADDR_MASK)) {
        vm_fail("the page table entry did not hold what was written to it");
        return false;
    }
    vm_flush_tlb();
    return true;
}

bool vm_map_page(uint64_t virtual_address, uint64_t physical_address)
{
    return vm_map_page_flags(virtual_address, physical_address, PTE_FLAGS);
}

bool vm_map_user_page(uint64_t virtual_address, uint64_t physical_address)
{
    return vm_map_page_flags(virtual_address, physical_address, PTE_USER_FLAGS);
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

/* Map a range of physical addresses somewhere in the mapping window, without
 * taking frames for it.
 *
 * This is for hardware.  A PCI base address register names a place in the
 * machine's physical address space, and to reach it the CPU needs that
 * physical address visible at some virtual address.  vm_alloc_pages cannot do
 * this because it allocates, and the address already exists: it belongs to
 * the device.
 *
 * Nothing here reserves the memory first.  The device claims the range through
 * its base address register, and firmware or the device model has already
 * decided it, so the right thing is to trust the BAR and map what it says.
 * A caller that wants the memory reserved should say so separately, and for
 * device memory that usually means asking the device not to use it yet. */
void *vm_map_physical(uint64_t physical_address, size_t bytes)
{
    uint64_t start;
    uint32_t count;
    uint64_t base = physical_address & ~(uint64_t)(VM_PAGE_BYTES - 1U);

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
        uint64_t physical = base + (uint64_t)index * VM_PAGE_BYTES;

        /* A partially mapped range is worse than none, because the caller has
         * no way to tell how much of it worked. */
        if (!vm_map_page(virtual_address, physical)) {
            for (uint32_t undo = 0; undo < index; ++undo) {
                vm_unmap_page(start + (uint64_t)undo * VM_PAGE_BYTES);
            }
            vm_fail("cannot map that range");
            return 0;
        }
    }
    vm_window_next += (uint64_t)count * VM_PAGE_BYTES;
    return (void *)(uintptr_t)start;
}

void vm_unmap_range(void *address, size_t bytes)
{
    uint64_t start = (uint64_t)(uintptr_t)address;
    uint32_t count = (uint32_t)((bytes + VM_PAGE_BYTES - 1U) / VM_PAGE_BYTES);

    for (uint32_t index = 0; index < count; ++index) {
        vm_unmap_page(start + (uint64_t)index * VM_PAGE_BYTES);
    }
    if (start + (uint64_t)count * VM_PAGE_BYTES == vm_window_next) {
        /* hand the tail of the window back, so a caller that maps and unmaps
         * in a loop does not run the window out */
        vm_window_next = start;
    }
}

void *vm_user_map_at(uint64_t virtual_address, size_t bytes)
{
    uint64_t first = virtual_address & ~(uint64_t)(VM_PAGE_BYTES - 1U);
    uint64_t count = (uint64_t)((bytes + VM_PAGE_BYTES - 1U) / VM_PAGE_BYTES);
    uint64_t last = first + count * VM_PAGE_BYTES;

    vm_error_text[0] = 0;
    if (bytes == 0U) {
        return 0;
    }
    if ((virtual_address % VM_PAGE_BYTES) != 0U) {
        vm_fail("the address must be page aligned");
        return 0;
    }
    /* The window has to exist before its bounds can be tested against, and
     * this is the first thing to touch it when a caller maps at an address it
     * chose rather than asking for an allocation.  Without this the limit is
     * still zero and every range is "not in the user window", which is the
     * whole range of it. */
    if (!vm_user_init()) {
        return 0;
    }
    if (last < first || last > vm_user_limit) {
        vm_fail("that range is not in the user window");
        return 0;
    }
    for (uint64_t at = first; at < last; at += VM_PAGE_BYTES) {
        void *frame = heap_alloc_frame();

        if (frame == 0) {
            for (uint64_t undo = first; undo < at; undo += VM_PAGE_BYTES) {
                uint64_t physical = vm_to_physical(undo);

                vm_unmap_page(undo);
                if (physical != 0U) {
                    heap_free_frame((void *)(uintptr_t)physical);
                }
            }
            vm_fail("out of memory for the pages");
            return 0;
        }
        {
            uint64_t *words = (uint64_t *)frame;

            for (uint32_t index = 0; index < VM_PAGE_BYTES / 8U; ++index) {
                words[index] = 0;
            }
        }
        if (!vm_map_user_page(at, (uint64_t)(uintptr_t)frame)) {
            heap_free_frame(frame);
            return 0;
        }
    }
    return (void *)(uintptr_t)first;
}

/* Where anonymous mmap starts handing out addresses.
 *
 * The window's bump pointer begins at the bottom of the window, which is where
 * a program's own segments are mapped -- those are placed by vm_user_map_at,
 * which deliberately does not move the pointer.  So mmap, which is a bump
 * allocation from that same pointer, would hand out addresses the program is
 * already using.  The loader calls this once it has finished placing the image,
 * the stack and the heap, and mmap starts above them. */
void vm_set_mmap_base(uint64_t address)
{
    if (address > vm_user_next && address <= vm_user_limit) {
        vm_user_next = address;
    }
}

void *vm_user_alloc_pages(size_t bytes)
{
    uint64_t start;
    uint32_t count;

    vm_error_text[0] = 0;
    if (bytes == 0U) {
        return 0;
    }
    if (!vm_user_init()) {
        return 0;
    }
    count = (uint32_t)((bytes + VM_PAGE_BYTES - 1U) / VM_PAGE_BYTES);
    if (vm_user_next + (uint64_t)count * VM_PAGE_BYTES > vm_user_limit) {
        vm_fail("the user mapping window is full");
        return 0;
    }
    start = vm_user_next;
    for (uint32_t index = 0; index < count; ++index) {
        uint64_t virtual_address = start + (uint64_t)index * VM_PAGE_BYTES;
        void *frame = heap_alloc_frame();

        if (frame == 0) {
            for (uint32_t undo = 0; undo < index; ++undo) {
                uint64_t done = start + (uint64_t)undo * VM_PAGE_BYTES;
                uint64_t physical = vm_to_physical(done);

                vm_unmap_page(done);
                if (physical != 0U) {
                    heap_free_frame((void *)(uintptr_t)physical);
                }
            }
            vm_fail("out of memory for the user pages");
            return 0;
        }
        {
            uint64_t *words = (uint64_t *)frame;

            for (uint32_t at = 0; at < VM_PAGE_BYTES / 8U; ++at) {
                words[at] = 0;
            }
        }
        if (!vm_map_user_page(virtual_address, (uint64_t)(uintptr_t)frame)) {
            heap_free_frame(frame);
            return 0;
        }
    }
    vm_user_next += (uint64_t)count * VM_PAGE_BYTES;
    return (void *)(uintptr_t)start;
}

void vm_user_free_pages(void *address, size_t bytes)
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
    if (vm_user_ready && start >= vm_user_base && start + (uint64_t)count * VM_PAGE_BYTES == vm_user_next) {
        vm_user_next = start;
    }
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

/* The leaf entry for a virtual address, flags included, for diagnostics.
 *
 * There is no other way to see why an access was refused.  A missing page and
 * a page that is present but not user accessible produce the same fault from
 * the program's point of view, and only the entry itself says which it was. */
/* Every level of the walk for an address, printed.
 *
 * A present PTE and a refused access can look identical from the program, and
 * the level at which the walk goes wrong is the whole question: the identity
 * map is set up by the boot code and works, so a walk that is correct for it
 * is not necessarily correct for the dynamic windows, which are built here. */
void vm_dump_walk(uint64_t virtual_address)
{
    uint32_t indices[4];
    uint64_t *table = vm_table_at(vm_read_cr3());
    static const char digits[] = "0123456789abcdef";

    indices[0] = (uint32_t)((virtual_address >> 39) & 0x1FFU);
    indices[1] = (uint32_t)((virtual_address >> 30) & 0x1FFU);
    indices[2] = (uint32_t)((virtual_address >> 21) & 0x1FFU);
    indices[3] = (uint32_t)((virtual_address >> 12) & 0x1FFU);
    for (int level = 0; level < 4; ++level) {
        uint64_t entry = table[indices[level]];

        serial_write("    walk");
        serial_putc((char)('0' + level));
        serial_write("[");
        serial_write_decimal(indices[level]);
        serial_write("] = 0x");
        for (int shift = 60; shift >= 0; shift -= 4) {
            serial_putc(digits[(entry >> shift) & 0xFU]);
        }
        serial_write(" user=");
        serial_putc((entry & PTE_USER) != 0U ? '1' : '0');
        serial_write(" present=");
        serial_putc((entry & PTE_PRESENT) != 0U ? '1' : '0');
        serial_putc('\n');
        if ((entry & PTE_PRESENT) == 0U || (entry & PTE_LARGE) != 0U) {
            return;
        }
        table = vm_table_at(vm_entry_address(entry));
    }
    {
        uint64_t leaf = table[indices[3]];

        serial_write("    leaf = 0x");
        for (int shift = 60; shift >= 0; shift -= 4) {
            serial_putc(digits[(leaf >> shift) & 0xFU]);
        }
        serial_write(" user=");
        serial_putc((leaf & PTE_USER) != 0U ? '1' : '0');
        serial_putc('\n');
    }
}

uint64_t vm_read_pte(uint64_t virtual_address)
{
    uint32_t pml4_index = (uint32_t)((virtual_address >> 39) & 0x1FFU);
    uint32_t pdpt_index = (uint32_t)((virtual_address >> 30) & 0x1FFU);
    uint32_t pd_index = (uint32_t)((virtual_address >> 21) & 0x1FFU);
    uint32_t pt_index = (uint32_t)((virtual_address >> 12) & 0x1FFU);
    uint64_t *level4 = vm_table_at(vm_read_cr3());
    uint64_t *level3;
    uint64_t *level2;
    uint64_t *level1;

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
    level1 = vm_table_at(vm_entry_address(level2[pd_index]));
    if ((level1[pt_index] & PTE_PRESENT) == 0U) {
        return 0;
    }
    return level1[pt_index];
}
