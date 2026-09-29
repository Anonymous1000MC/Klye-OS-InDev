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
    while (done < filesz) {
        char buffer[4096];
        uint32_t want = (uint32_t)(sizeof(buffer) < (filesz - done)
                                       ? sizeof(buffer) : (filesz - done));
        int got = vfs_read_at(path, buffer, (uint32_t)(offset + done), want);

        if (got <= 0) {
            elf_fail("the file ended in the middle of a segment");
            return false;
        }
        {
            char *where = (char *)(uintptr_t)(virtual_address + done);
            uint32_t index = 0;

            while (index < (uint32_t)got) {
                where[index] = buffer[index];
                index++;
            }
            done += (uint32_t)got;
        }
    }
    /* the rest of the segment exists in memory but not in the file, and a
     * program is entitled to read it as zero */
    end = virtual_address + memsz;
    for (uint64_t at = virtual_address + filesz; at < end; at++) {
        *(volatile uint8_t *)(uintptr_t)at = 0U;
    }
    return true;
}

/* Move a position independent binary to where it was linked.
 *
 * ET_DYN is a static PIE: it is linked to run at some base and carries no
 * absolute addresses, everything being relative.  So nothing has to be
 * rewritten -- the program will do its own relocations against wherever it
 * lands -- but it does need to be told where that is, which is the load bias
 * in the auxiliary vector.
 *
 * The relocations are therefore deliberately NOT applied here.  A static
 * PIE applies its own R_X86_64_RELATIVE entries in its startup code, because
 * the base is not known until load time.  Applying them here as well would
 * apply them twice, and the second application would be wrong by the bias. */
static void elf_apply_internal_relocations(void)
{
    /* Intentionally empty; see above. */
}

bool elf_run(const char *path)
{
    struct elf64_header header;
    uint64_t bias = 0;
    uint64_t stack;
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

    /* A stack for it.  8 KiB is what a program that only makes syscalls needs
     * and is deliberately not configurable yet: a real C library will want
     * more, and a stack that is too small is a fault in the middle of the
     * program's own code rather than a message here. */
    {
        void *pages = vm_user_alloc_pages(ELF_STACK_BYTES);

        if (pages == 0) {
            elf_fail("no memory for the program stack");
            return false;
        }
        stack = ((uint64_t)(uintptr_t)pages + ELF_STACK_BYTES) & ~0xFULL;
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
