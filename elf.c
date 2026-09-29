/* elf.c - load an ELF64 executable and start it in ring 3.
 *
 * The ring 3 machinery already works: a program in ring 3 can make a syscall,
 * the kernel runs a handler for it, and the return lands back in the program
 * (see ring3.S, and session-notes/ring3.md for how long that took to get
 * right).  What was missing was a way to get a program *into* ring 3, which is
 * this file.
 *
 * The scope is a static, position independent, x86-64 executable that talks to
 * the kernel through the syscall path.  That is enough to run a real compiled
 * program and it is the only kind this can honestly claim to support: there is
 * no dynamic linker here, no TLS, and no signal or futex machinery, so a
 * program linked against a C library gets as far as libc's own startup and then
 * stops.  Each of those is listed in TODO.md rather than faked here.
 */

#include "elf.h"

#include "kernel.h"
#include "mmu.h"
#include "scheduler.h"
#include "user.h"
#include "vfs.h"


/* From the ELF64 specification.  Only the fields that are read are named; the
 * rest of each structure is skipped by size, not by a copy of every field,
 * because a copy of every field is a second place for the format to be wrong
 * in. */
struct elf64_ident {
    uint8_t magic[4];
    uint8_t class;
    uint8_t data;
    uint8_t version;
    uint8_t osabi;
    uint8_t padding[8];
};

struct elf64_header {
    struct elf64_ident ident;
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct elf64_program_header {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
};

struct elf64_rel {
    uint64_t offset;
    uint64_t info;
};

struct elf64_rela {
    uint64_t offset;
    uint64_t info;
    int64_t addend;
};

#define ET_EXEC 2
#define ET_DYN  3
#define EM_X86_64 62

#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3

#define PF_X 1
#define PF_W 2
#define PF_R 4

/* Relocation types that a position independent x86-64 binary actually uses.
 * The architecture has more; these are the ones glibc and musl emit. */
#define R_X86_64_RELATIVE 8
#define R_X86_64_GLOB_DAT  6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_IRELATIVE 37

#define R_X86_64_TYPE(info)  ((uint32_t)((info) & 0xFFFFFFFFU))
#define R_X86_64_SYM(info)   ((uint32_t)((info) >> 32))

static char elf_error_text[128] = "";

static void elf_fail(const char *why)
{
    __builtin_strncpy(elf_error_text, why, sizeof(elf_error_text) - 1);
    elf_error_text[sizeof(elf_error_text) - 1] = 0;
}

const char *elf_error(void)
{
    return elf_error_text;
}

static void elf_report(const char *what, uint64_t value)
{
    serial_write("  elf: ");
    serial_write(what);
    serial_write(" ");
    serial_write_decimal(value);
    serial_putc('\n');
}

/* True when [start, end) is inside the region the program may use.
 *
 * Every address a loaded program touches comes out of its own headers, and a
 * malformed or hostile file can put a segment anywhere at all, including
 * inside the page tables or over the kernel's own text.  Checking the range
 * here means a bad file is rejected with a message rather than quietly
 * replacing the kernel. */
#define ELF_MIN_ADDRESS 0x100000ULL          /* 1 MiB: above the low mappings */
#define ELF_MAX_ADDRESS 0x00007FFFFFFFF000ULL /* the user half, canonical */

static bool elf_range_ok(uint64_t start, uint64_t size)
{
    if (start < ELF_MIN_ADDRESS || start >= ELF_MAX_ADDRESS) {
        return false;
    }
    if (size > ELF_MAX_ADDRESS - start) {
        return false;
    }
    return true;
}

/* Make [start, start+size) writable from ring 3, at the address asked for.
 *
 * A program does not get to choose where it lives, so this cannot be the bump
 * allocator: the segments of a PIE are placed at the same offset from one base,
 * and a non-PIE is mapped exactly where it was linked. */
static bool elf_map_user(uint64_t start, uint64_t size)
{
    uint64_t first = start & ~(uint64_t)(VM_PAGE_BYTES - 1U);
    uint64_t last = (start + size + VM_PAGE_BYTES - 1U) & ~(uint64_t)(VM_PAGE_BYTES - 1U);

    if (last < first) {
        elf_fail("a segment wraps the address space");
        return false;
    }
    if (vm_user_map_at(first, (size_t)(last - first)) == 0) {
        elf_fail(vm_error());
        return false;
    }
    return true;
}

static void elf_note(const char *what)
{
    serial_write("  elf: ");
    serial_write(what);
    serial_putc('\n');
}

/* Copy a PT_LOAD segment into place and zero the part that has no file
 * content, which is what memsz larger than filesz means. */
static bool elf_load_segment(const char *path, const struct elf64_program_header *ph)
{
    uint64_t virtual_address = ph->vaddr;
    uint64_t memsz = ph->memsz;
    uint64_t filesz = ph->filesz;
    uint64_t offset = ph->offset;
    uint64_t end;
    uint32_t done = 0;

    if (memsz == 0U) {
        return true;
    }
    if (!elf_range_ok(virtual_address, memsz)) {
        elf_note("a segment is outside the address range a program may use");
        return false;
    }
    if (filesz > memsz) {
        elf_note("a segment claims more file than memory");
        return false;
    }
    if (!elf_map_user(virtual_address & ~(uint64_t)(VM_PAGE_BYTES - 1U),
                      memsz + (virtual_address & (VM_PAGE_BYTES - 1U)))) {
        return false;
    }
    /* The copy goes through the physical address, not the virtual one.
     *
     * The kernel runs with paging on and the user window is not identity
     * mapped, so a store to a program's virtual address from ring 0 does not
     * land in the program's page: it either faults or hits whatever the
     * kernel's own mapping of that number happens to be.  The segments came up
     * present and user accessible and still contained nothing, because the
     * bytes were never written to the frames the page table points at.
     *
     * A page at a time, and each page translated on its own, because a
     * segment can start part way into a page and a translation is only valid
     * for the page it came from. */
    while (done < filesz) {
        char buffer[4096];
        uint32_t want = (uint32_t)(sizeof(buffer) < (filesz - done)
                                       ? sizeof(buffer) : (filesz - done));
        int got = vfs_read_at(path, buffer, (uint32_t)(offset + done), want);
        uint32_t index = 0;

        if (got <= 0) {
            elf_fail("the file ended in the middle of a segment");
            return false;
        }
        while (index < (uint32_t)got) {
            uint64_t at = virtual_address + done + index;
            uint64_t physical = vm_to_physical(at);

            if (physical == 0U) {
                elf_fail("a segment page is not mapped after mapping it");
                return false;
            }
            *((volatile uint8_t *)(uintptr_t)physical) =
                (uint8_t)buffer[index];
            index++;
        }
        done += (uint32_t)got;
    }
    /* the rest of the segment exists in memory but not in the file, and a
     * program is entitled to read it as zero.  heap_alloc_frame hands back
     * zeroed pages, so this only has to cover a segment that overlaps a page
     * an earlier segment already filled. */
    end = virtual_address + memsz;
    for (uint64_t at = virtual_address + filesz; at < end; at++) {
        uint64_t physical = vm_to_physical(at);

        if (physical != 0U) {
            *((volatile uint8_t *)(uintptr_t)physical) = 0U;
        }
    }
    return true;
}

/* Move a position independent binary to where it was linked.
 *
 * ET_DYN is a static PIE: it is linked to run at some base and carries no
 * absolute addresses, everything being relative.  So nothing has to be
 * rewritten -- but it does need to be told where that is, which is the load
 * bias in the auxiliary vector.
 *
 * The relocations are deliberately NOT applied here, and that is not an
 * omission.  A static PIE applies its own: glibc's static-pie carries
 * _dl_relocate_static_pie and calls it from __libc_start_main, so the C
 * runtime walks its own .rela.dyn after the kernel has mapped the segments and
 * before it reaches main.  Linux does not process relocations for these
 * binaries for the same reason.
 *
 * Measured on a glibc -static-pie built to check, which has 1125 entries: 1104
 * R_X86_64_RELATIVE followed by 21 R_X86_64_IRELATIVE, all 21 of them
 * targeting addresses inside the span the RELATIVE ones patch.  Applying them
 * here would mean each is applied once by the kernel and again by glibc, and
 * the second application would be wrong by the load bias -- a data section
 * pointing into the middle of nowhere, in a program that had otherwise loaded
 * correctly.
 *
 * This was nearly implemented on the strength of advice that a static PIE does
 * not self-relocate.  The advice was wrong; the symbol table said so. */
static void elf_apply_internal_relocations(void)
{
    /* Intentionally empty, and see above for why it has to stay that way. */
    (void)elf_apply_internal_relocations;
}

/* Auxiliary vector entries.
 *
 * The handful that a C library reads during startup and treats as required.
 * AT_RANDOM is not optional in practice: it points at sixteen bytes the
 * library uses to seed its stack protector, so a zero there is read as a
 * pointer to address zero. */
#define AT_NULL    0
#define AT_PHDR    3
#define AT_PHENT   4
#define AT_PHNUM   5
#define AT_PAGESZ  6
#define AT_BASE    7
#define AT_ENTRY   9
#define AT_UID     11
#define AT_EUID    12
#define AT_GID     13
#define AT_EGID    14
#define AT_HWCAP   16
#define AT_RANDOM  25

/* The environment a program starts with.
 *
 * Deliberately near empty.  Anything here is read by the C library during
 * startup -- both musl and glibc walk envp looking for LD_PRELOAD, the locale
 * and similar -- so a variable that means something to either of them would
 * change how a program behaves before main runs.  PATH is the one worth
 * having: a shell without it cannot find anything it is asked to run, and every
 * program reads it as a plain string. */
static const char *const initial_environment[] = {
    "PATH=/bin:/usr/bin",
    0
};

/* Pairs in the auxiliary vector below, the terminating AT_NULL included.
 *
 * This has to be the number of pairs actually written at the bottom of this
 * function, not an estimate.  It sizes the space the vector occupies, and
 * AT_RANDOM's sixteen bytes are placed immediately after it, so one pair too
 * few here puts those bytes on top of the last pair written -- which is the
 * AT_NULL terminator.  A vector whose terminator has been overwritten with a
 * random number is not a vector that ends: a library walking it runs off the
 * end of the stack and faults. */
#define ELF_AUX_PAIRS 11U

/* Write eight bytes to a user address, by physical address.
 *
 * The same reason the segment copy works this way: the kernel cannot store
 * through a ring 3 virtual address, because the user window is not identity
 * mapped.  Returns false when the address is not mapped, which is worth knowing
 * rather than quietly writing somewhere else. */
static bool elf_store(uint64_t virtual_address, uint64_t value)
{
    uint64_t physical = vm_to_physical(virtual_address);

    if (physical == 0U) {
        return false;
    }
    *((volatile uint64_t *)(uintptr_t)physical) = value;
    return true;
}

static bool elf_store_aux(uint64_t *at, uint64_t type, uint64_t value)
{
    if (!elf_store(*at, type) || !elf_store(*at + 8U, value)) {
        return false;
    }
    *at += 16U;
    return true;
}

/* Build the stack a C library expects to be entered on.
 *
 * From low addresses up, at the moment control reaches the entry point:
 *
 *     argc              argument count
 *     argv[0] ..        pointers to the argument strings
 *     NULL              end of argv
 *     envp[0] ..        pointers to the environment strings
 *     NULL              end of envp
 *     auxv pairs        (type, value), ended by AT_NULL
 *     AT_RANDOM bytes   sixteen bytes the library seeds its protector from
 *     strings           what argv and envp point at
 *
 * and rsp points at argc.  A library walks the two NULLs and then the vector
 * with no bounds of any kind, so every one of them has to be exactly where it
 * is expected.  A vector missing its terminator walks off the top of the stack;
 * a missing NULL after argv means envp is read as a further argument, and the
 * first "environment variable" is a wild pointer.
 *
 * The strings are written after the pointers that refer to them, and the
 * cursor that tracks where each string will land advances in step with the
 * pointers, so there is one place that decides the layout rather than two that
 * have to agree about it.
 *
 * Returned is the stack pointer to enter the program on, or 0 if any of it
 * could not be written. */
static uint64_t elf_build_stack(uint64_t stack_top, const char *program,
                                const struct elf64_header *header,
                                uint64_t bias, uint64_t phdr_vaddr)
{
    uint32_t environment_count = 0U;
    uint64_t fixed_words;
    uint64_t words;
    uint64_t at;
    uint64_t bottom;
    uint64_t strings;
    uint64_t string_cursor;
    uint64_t random_at;
    uint32_t index;

    while (initial_environment[environment_count] != 0) {
        environment_count++;
    }

    /* Everything below the strings: argc, argv[0], argv's NULL, envp, envp's
     * NULL, the vector, and the sixteen random bytes.  Counted exactly, so the
     * strings land in a known place rather than wherever the arithmetic
     * happened to leave them. */
    fixed_words = 1U                                   /* argc */
                + 1U                                   /* argv[0] */
                + 1U                                   /* argv's NULL */
                + (uint64_t)environment_count         /* envp */
                + 1U                                   /* envp's NULL */
                + 2ULL * ELF_AUX_PAIRS                /* auxv */
                + 2U;                                  /* AT_RANDOM's bytes */
    words = fixed_words + 64U;                         /* strings, generously */
    if (words * 8U > ELF_STACK_BYTES) {
        return 0;
    }

    /* Bottom of the layout, aligned down to sixteen.  The mask goes on the
     * sum rather than on the top alone: the stack pointer is the bottom, so
     * aligning only the top leaves the bottom misaligned by the remainder. */
    at = (stack_top - words * 8U) & ~0xFULL;
    if (at + words * 8U > stack_top) {
        return 0;                      /* would run off the bottom */
    }
    /* Remembered, because `at` walks upward as the layout is written and the
     * stack pointer to return is this one, not wherever the walk finishes. */
    bottom = at;
    strings = at + fixed_words * 8U;
    string_cursor = strings;
    random_at = at + (fixed_words - 2U) * 8U;

    if (!elf_store(at, 1U)) {          /* argc: the program name only */
        return 0;
    }
    at += 8U;
    if (!elf_store(at, string_cursor)) {        /* argv[0] */
        return 0;
    }
    string_cursor += __builtin_strlen(program) + 1U;
    at += 8U;
    if (!elf_store(at, 0U)) {          /* end of argv */
        return 0;
    }
    at += 8U;
    for (index = 0; index < environment_count; ++index) {
        if (!elf_store(at, string_cursor)) {
            return 0;
        }
        string_cursor += __builtin_strlen(initial_environment[index]) + 1U;
        at += 8U;
    }
    if (!elf_store(at, 0U)) {          /* end of envp */
        return 0;
    }
    at += 8U;

    /* The auxiliary vector.
     *
     * AT_PHDR points at the program headers *as they are in memory*, which for
     * a PIE means biased by the load address.  A library handed the unrelocated
     * value reads its own headers from the wrong place, and every field in
     * them -- the entry point included -- comes back wrong.
     *
     * AT_RANDOM is not optional in practice: it points at sixteen bytes the
     * library reads to seed its stack protector, so a zero there is read as a
     * pointer to address zero. */
    if (!elf_store_aux(&at, AT_PHDR, bias + phdr_vaddr) ||
        !elf_store_aux(&at, AT_PHENT, (uint64_t)header->phentsize) ||
        !elf_store_aux(&at, AT_PHNUM, (uint64_t)header->phnum) ||
        !elf_store_aux(&at, AT_PAGESZ, (uint64_t)VM_PAGE_BYTES) ||
        !elf_store_aux(&at, AT_ENTRY, bias + header->entry) ||
        !elf_store_aux(&at, AT_RANDOM, random_at) ||
        !elf_store_aux(&at, AT_UID, 0U) ||
        !elf_store_aux(&at, AT_EUID, 0U) ||
        !elf_store_aux(&at, AT_GID, 0U) ||
        !elf_store_aux(&at, AT_HWCAP, 0U) ||
        !elf_store_aux(&at, AT_NULL, 0U)) {
        return 0;
    }

    /* The sixteen bytes AT_RANDOM points at.  There is no entropy source here,
     * so this is a fixed non-zero pattern rather than a claim of randomness;
     * all zeroes would leave a stack protector that is trivially guessed. */
    at = random_at;
    for (index = 0; index < 2U; ++index) {
        if (!elf_store(at, 0x9E3779B97F4A7C15ULL * (uint64_t)(index + 1U))) {
            return 0;
        }
        at += 8U;
    }

    /* The strings, at the addresses the pointers above already name. */
    at = strings;
    if (!elf_store(at, (uint64_t)(uintptr_t)program)) {
        return 0;
    }
    at += __builtin_strlen(program) + 1U;
    for (index = 0; index < environment_count; ++index) {
        if (!elf_store(at, (uint64_t)(uintptr_t)initial_environment[index])) {
            return 0;
        }
        at += __builtin_strlen(initial_environment[index]) + 1U;
    }

    /* The stack pointer is the *bottom* of the layout, where argc is.
     *
     * It was returned as strings - 8, on the reasoning that rsp is "the word
     * below argv[0]".  argv[0] is the second word of the layout, so the word
     * below it is the bottom -- but strings is not argv[0], it is where the
     * strings were placed, hundreds of words up.  The program was therefore
     * entered 200 bytes above its own argc, reading the middle of the
     * auxiliary vector as argc, argv and envp.
     *
     * The symptom was a fault inside glibc's __libc_start_main walking envp
     * looking for its NULL terminator, at a wild address: with argc and the
     * argument pointers read out of the middle of the vector, the computed
     * envp is nowhere near the one that was built. */
    return bottom;
}

bool elf_run(const char *path)
{
    struct elf64_header header;
    uint64_t bias = 0;
    uint64_t stack = 0U;
    uint64_t stack_region_top;
    uint64_t image_end = 0U;
    uint64_t phdr_page = 0xFFFFFFFFFFFFFFFFULL;
    bool have_phdr_page = false;
    int got;
    uint16_t index;

    elf_error_text[0] = 0;

    got = vfs_read(path, (char *)&header, (uint32_t)sizeof(header));
    if (got < (int)sizeof(header)) {
        elf_fail("the file is too small to be an ELF executable");
        return false;
    }
    if (header.ident.magic[0] != 0x7FU || header.ident.magic[1] != 'E' ||
        header.ident.magic[2] != 'L' || header.ident.magic[3] != 'F') {
        elf_fail("the file is not an ELF executable");
        return false;
    }
    if (header.ident.class != 2U) {
        elf_fail("only 64-bit ELF is supported");
        return false;
    }
    if (header.ident.data != 1U) {
        elf_fail("only little-endian ELF is supported");
        return false;
    }
    if (header.machine != EM_X86_64) {
        elf_fail("the executable is not for this machine");
        return false;
    }
    if (header.type != ET_EXEC && header.type != ET_DYN) {
        elf_fail("the file is not an executable or a shared object");
        return false;
    }
    /* The entry point is checked after the bias is added, below.  A position
     * independent binary's header entry is 0x1000, which is meaningless on
     * its own: it is an offset from wherever the program is loaded, so testing
     * it against a minimum address rejects every PIE ever built. */
    if (header.phnum == 0U) {
        elf_fail("the executable has no program headers");
        return false;
    }
    if (header.phentsize != sizeof(struct elf64_program_header)) {
        elf_fail("unexpected program header size");
        return false;
    }
    if (header.type == ET_DYN) {
        /* Every vaddr in a PIE is an offset from zero, and the whole thing is
         * mapped together, so one bias covers it.  It has to be page aligned
         * or the lowest segment is mapped at an address it was not linked for. */
        bias = USER_ELF_LOAD_BIAS;
        if ((bias & (VM_PAGE_BYTES - 1U)) != 0U) {
            elf_fail("the load address is not page aligned");
            return false;
        }
    }
    if ((header.entry + bias) < ELF_MIN_ADDRESS ||
        (header.entry + bias) >= ELF_MAX_ADDRESS) {
        elf_fail("the entry point is outside the address range a program may use");
        return false;
    }
    /* The highest address any segment reaches, so the stack can go above it.
     *
     * Read out of the program headers before anything is mapped, because the
     * stack is a bump allocation from the same window the segments were mapped
     * into and the two must not overlap. */
    for (index = 0; index < header.phnum; ++index) {
        struct elf64_program_header ph;
        uint32_t at = (uint32_t)(header.phoff +
                                 (uint64_t)index * (uint64_t)header.phentsize);
        int read = vfs_read_at(path, (char *)&ph, at, (uint32_t)sizeof(ph));

        if (read < (int)sizeof(ph)) {
            elf_fail("the program header table is truncated");
            return false;
        }
        if (ph.type != PT_LOAD || ph.memsz == 0U) {
            continue;
        }
        if ((ph.flags & (PF_R | PF_W | PF_X)) == 0U) {
            continue;
        }
        if (ph.vaddr + ph.memsz > image_end) {
            image_end = ph.vaddr + ph.memsz;
        }
        /* The page the program header table itself lands on.  AT_PHDR has to
         * point at the headers where they will be in memory, and they are only
         * mapped because some segment covers them, so the page is the lowest
         * one any segment starts on. */
        if (!have_phdr_page ||
            (ph.vaddr & ~(uint64_t)(VM_PAGE_BYTES - 1U)) < phdr_page) {
            phdr_page = ph.vaddr & ~(uint64_t)(VM_PAGE_BYTES - 1U);
            have_phdr_page = true;
        }
    }
    /* Above the last segment, rounded up to a page, plus a gap.
     *
     * vm_user_map_at, which is what maps the segments, deliberately does not
     * move the window's bump pointer, so a plain vm_user_alloc_pages for the
     * stack hands back the bottom of the window -- the same pages the entry
     * point was just loaded into.  Allocating the stack then zeroes every page
     * it covers, and the program's own text was wiped moments after being
     * written: the page stayed present and user readable and contained nothing,
     * so the entry faulted on an instruction fetch of zeroes and the cause was
     * nowhere near the stack that caused it. */
    image_end = (image_end + VM_PAGE_BYTES - 1U) & ~(uint64_t)(VM_PAGE_BYTES - 1U);
    image_end += VM_PAGE_BYTES;
    for (index = 0; index < header.phnum; ++index) {
        struct elf64_program_header ph;
        uint32_t at = (uint32_t)(header.phoff +
                                 (uint64_t)index * (uint64_t)header.phentsize);
        int read = vfs_read_at(path, (char *)&ph, at, (uint32_t)sizeof(ph));

        if (read < (int)sizeof(ph)) {
            elf_fail("the program header table is truncated");
            return false;
        }
        if (ph.type == PT_INTERP) {
            elf_fail("dynamically linked executables are not supported yet");
            return false;
        }
        if (ph.type != PT_LOAD) {
            continue;
        }
        if ((ph.flags & (PF_R | PF_W | PF_X)) == 0U) {
            continue;
        }
        {
            struct elf64_program_header moved = ph;

            moved.vaddr += bias;
            if (!elf_load_segment(path, &moved)) {
                return false;
            }
        }
    }
    elf_apply_internal_relocations();

    /* A stack for it.  64 KiB is what a program that only makes syscalls needs
     * and is deliberately not configurable yet: a real C library will want more,
     * and a stack that is too small is a fault in the middle of the program's
     * own code rather than a message here. */
    {
        uint64_t low = image_end + bias;
        uint64_t high = low + ELF_STACK_BYTES;

        /* Mapped at an address chosen here rather than bump allocated, because
         * the window's own pointer is still at the bottom and the segments own
         * that.  Rounded down to a page: the segments were rounded up when the
         * end was computed, so this is already aligned, and asking for an
         * unaligned range is refused rather than quietly rounded. */
        if (vm_user_map_at(low & ~(uint64_t)(VM_PAGE_BYTES - 1U),
                           (size_t)ELF_STACK_BYTES) == 0) {
            elf_fail(vm_error());
            return false;
        }
        /* The stack pointer is the top of the region and grows down, so it is
         * the first address *past* the last usable byte, not a usable address
         * itself.  Sixteen byte aligned because that is what the ABI wants at
         * the point a program is entered. */
        /* The top of the stack region, kept: the stack pointer returned by
         * elf_build_stack points *into* the middle of it, and the heap goes
         * above the whole thing rather than above that pointer.  Adding the
         * size to the entry pointer instead would start the heap in the middle
         * of the stack, and a program that grew into it would overwrite its
         * own arguments. */
        stack_region_top = high & ~0xFULL;
        /* The entry stack is not just a stack pointer: a C library is entered
         * on argc, argv, envp and an auxiliary vector, and walks all three with
         * no bounds.  Built here, below the segments, and the pointer into the
         * middle of it is what the program starts on. */
        if (!have_phdr_page) {
            elf_fail("the program has no loadable segment for its headers");
            return false;
        }
        stack = elf_build_stack(stack_region_top, path, &header, bias,
                                header.phoff);
        if (stack == 0U) {
            elf_fail("the initial stack could not be built");
            return false;
        }
        /* A heap above the stack, reserved rather than grown: brk moves a break
         * inside a region the program owns and does not map more on demand, so
         * the region has to exist before the program asks for it. */
        {
            uint64_t heap_low = stack_region_top;
            uint64_t heap_high = heap_low + ELF_HEAP_BYTES;

            if (vm_user_map_at(heap_low & ~(uint64_t)(VM_PAGE_BYTES - 1U),
                               (size_t)ELF_HEAP_BYTES) == 0) {
                elf_fail(vm_error());
                return false;
            }
            user_set_heap(heap_low, heap_high);
            /* mmap is a bump allocation from the same window, and that window's
             * pointer still points at the bottom, where this program's own
             * segments are.  Tell it where the program ends. */
            vm_set_mmap_base(heap_high);
        }
    }

    elf_report("entry", header.entry + bias);
    elf_report("stack", stack);
    elf_report("bias", bias);
    /* A fetch of the entry is refused with error 5 -- present, user,
     * protection -- when the page is mapped but the processor will not execute
     * it.  The entry's physical page is printed so the two can be told apart:
     * a zero here means the segment was never mapped, and a real address with
     * the fault means the mapping is there and something about it is wrong. */
    elf_report("entry physical", vm_to_physical(header.entry + bias));
    /* Read the leaf entry back and print it, and do the same for a page the
     * ring 3 test uses and is known to execute from.  Comparing the two is
     * the only way to tell a page that was never mapped from one that was
     * mapped and then refused, which look identical from the fault. */
    {
        extern uint64_t vm_read_pte(uint64_t virtual_address);

        elf_report("entry pte", vm_read_pte(header.entry + bias));
        elf_report("test pte", vm_read_pte(USER_ELF_LOAD_BIAS));
        {
            extern void vm_dump_walk(uint64_t virtual_address);

            serial_write("  elf: walk for the entry\n");
            vm_dump_walk(header.entry + bias);
            serial_write("  elf: walk for a known good page\n");
            vm_dump_walk(USER_ELF_LOAD_BIAS);
        }
    }

    if (task_spawn_user(header.entry + bias, stack) < 0) {
        elf_fail("no free task slot for the program");
        return false;
    }
    return true;
}
