/* user.c - ring 3, and the syscall the ABI is built on.
 *
 * This is the first program to run outside the kernel.  Nothing here is
 * finished and nothing here is a Linux binary loader: the point is that a
 * program in ring 3 can make a call into the kernel and get an answer back,
 * which is the whole of what every later step needs and none of what any of
 * them are.
 */

#include "user.h"

#include "kernel.h"

#include "io.h"
#include "mmu.h"
#include "scheduler.h"
#include "kernel.h"

/* Selectors.  The ring 3 halves matter: a segment selector carries the ring
 * in its low two bits, so the same descriptor is 0x10 in the kernel and 0x13
 * in a user program, and loading the wrong one is a general protection fault
 * rather than anything informative. */
#define KERNEL_CODE 0x08U
#define KERNEL_DATA 0x10U
#define USER_DATA   0x18U   /* 0x13 from ring 3 */
#define USER_CODE   0x20U   /* 0x23 from ring 3 */

static uint64_t syscalls;
static uint64_t user_exit_code;

/* Linux's syscall numbers, so that the numbering does not have to be changed
 * later when a real binary arrives.  Only the few a first program needs are
 * here. */
#define SYS_WRITE     1
#define SYS_MMAP      9
#define SYS_BRK      12
#define SYS_ARCH_PRCTL 158
#define SYS_EXIT     60

/* arch_prctl subcommands, and the one that matters here.
 *
 * ARCH_SET_FS is how a process sets the thread pointer: the base of its thread
 * local storage.  It is not optional for a C library, because errno lives there
 * and so does the stack protector's guard value, and both are touched during
 * startup rather than at some point the program chooses.  A library that
 * cannot set FS faults inside its own initialisation, before it has said
 * anything useful. */
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_SET_GS 0x1001

/* The thread pointer of the running user program, kept in a kernel global
 * because MSR_FS_BASE is per-thread hardware state and there is one user
 * program at a time.  The kernel's own FS is not this: writing the MSR here
 * would repoint the kernel's own thread pointer at a user address, and the
 * first kernel access to its TLS would read a user page. */
/* The slot itself is defined in ring3.S, next to the other values the
 * transitions need, so there is one copy of it. */
extern volatile uint64_t user_fs_base_slot;
#define user_fs_base user_fs_base_slot

/* The program break, and the first address its heap may use.
 *
 * brk grows and shrinks a program's heap in one contiguous region, which Linux
 * implements as a single mapping the kernel extends on demand.  Here the region
 * is a fixed reservation the loader mapped, and brk only moves the break
 * inside it: extending past the end is refused with the current break rather
 * than mapping more, because a heap that can grow into whatever it likes is a
 * way for one program to map over another's pages. */
static uint64_t user_break;
static uint64_t user_break_end;

/* Called by the loader once the program's address space exists, so brk knows
 * what region it is allowed to hand out. */
void user_set_heap(uint64_t start, uint64_t end)
{
    user_break = start;
    user_break_end = end;
}

/* mmap, for anonymous private mappings only.
 *
 * A C library asks for memory during startup with addr 0, prot
 * PROT_READ|PROT_WRITE, flags MAP_PRIVATE|MAP_ANONYMOUS, fd -1 and offset 0,
 * and expects back a page-aligned address it can read and write, backed by
 * zeroed pages.  Answering -ENOSYS is a lie in a different direction: the
 * library cannot allocate, and a program that allocates during startup has
 * nowhere to go.
 *
 * File-backed mappings need a descriptor table and a page cache and are not
 * here; they are refused rather than faked, so a program gets a real error
 * instead of an address that reads the wrong file. */
#define PROT_READ   0x1U
#define PROT_WRITE  0x2U
#define MAP_PRIVATE   0x0002U
#define MAP_ANONYMOUS 0x0020U
#define MAP_FIXED     0x0010U

static int user_do_mmap(struct interrupt_registers *regs)
{
    uint64_t length = regs->rsi;
    uint64_t prot = regs->rdx;
    uint64_t flags = regs->r10;
    int64_t fd = (int64_t)regs->r8;
    uint64_t offset = regs->r9;

    if ((flags & MAP_ANONYMOUS) == 0U || fd >= 0) {
        regs->rax = (uint64_t)-95;   /* EOPNOTSUPP: no file mappings yet */
        return 0;
    }
    if (offset != 0U) {
        regs->rax = (uint64_t)-22;   /* EINVAL */
        return 0;
    }
    if (length == 0U) {
        regs->rax = (uint64_t)-22;
        return 0;
    }
    if ((prot & ~(uint64_t)(PROT_READ | PROT_WRITE)) != 0U) {
        regs->rax = (uint64_t)-22;   /* no PROT_EXEC yet */
        return 0;
    }
    if ((flags & MAP_FIXED) != 0U) {
        /* A fixed mapping would overwrite whatever is there, and the one thing
         * this window must not do is hand out a page a program is already
         * using.  Refused until there is a way to check. */
        regs->rax = (uint64_t)-22;
        return 0;
    }
    {
        void *pages = vm_user_alloc_pages(length);

        if (pages == 0) {
            regs->rax = (uint64_t)-12;   /* ENOMEM */
            return 0;
        }
        /* MAP_PRIVATE|MAP_ANONYMOUS on a fresh zeroed frame needs no copy and
         * no per-page state: every mapping of it starts identical, so two
         * private anonymous mappings of the same file offset would be
         * indistinguishable.  That is not right in general, and it is fine
         * here because nothing in this kernel can fault a page in yet, so no
         * two live mappings can ever diverge. */
        (void)(MAP_PRIVATE);
        regs->rax = (uint64_t)(uintptr_t)pages;
    }
    return 0;
}

static int user_do_brk(uint64_t wanted)
{
    if (wanted == 0U) {
        return 0;                       /* asking, not telling */
    }
    /* A break below the start means shrink; the pages stay mapped, which is
     * what Linux does too and is why a heap that shrinks and grows again does
     * not have to be remapped. */
    if (wanted < user_break_end && wanted >= user_break - (user_break & 0xFFFU)) {
        user_break = wanted;
        return 0;
    }
    if (wanted > user_break_end) {
        /* Out of the region.  Report where the break actually is, which is
         * what Linux does and is what a library checks. */
        return (int)user_break;
    }
    user_break = wanted;
    return 0;
}

static const char *hex = "0123456789abcdef";

static void put_hex(uint64_t value)
{
    char digits[17];
    int length = 0;

    if (value == 0U) {
        digits[length++] = '0';
    }
    while (value != 0U && length < 16) {
        digits[length++] = hex[value & 0xFU];
        value >>= 4;
    }
    while (length > 0) {
        serial_putc(digits[--length]);
    }
}

/* Markers along the ring 3 path, printed over the serial port.
 *
 * A fault in this path can end in a triple fault, and a triple fault resets
 * the processor with the fault handler unable to run, so the panic screen
 * shows nothing at all.  Without a trace there is no way to tell which of the
 * several steps failed, and each of them looks identical from outside: the
 * machine reboots. */
void serial_marker(const char *text)
{
    serial_write(text);
    serial_write("\n");
}

/* The ring 3 path calls these by address, because the markers are written
 * where the path can call them and setting the text here keeps the strings
 * next to the code that decides when they happen. */
#define MARK(name, text)                              \
    void name(void) { serial_marker(text); }

MARK(serial_marker_ptr_enter, "r3: user_enter, about to iretq into ring 3");
MARK(serial_marker_ptr_in_kernel, "r3: now in ring 0 after lretq");
MARK(serial_marker_ptr_return, "r3: returning to ring 3");

/* Every syscall, with its arguments and its result.
 *
 * Reading a C library's startup one fault at a time is hopeless: each fault
 * reveals exactly one missing piece and hides the next, and the fix for one is
 * only visible after the previous one is in.  Printing the whole sequence
 * turns that into a list, so the missing pieces can all be implemented
 * together instead of one per run.
 *
 * The result is printed by the return path rather than here, so a syscall that
 * never returns -- and exit never does -- is still on the record. */
#define SYS_EXIT_GROUP 231

static void syscall_dispatch(struct interrupt_registers *regs,
                              uint64_t number);

static bool syscall_trace = false;
static uint64_t traced_number;

static void trace_begin(struct interrupt_registers *regs, uint64_t number)
{
    traced_number = number;
    serial_write("TRACE rax=");
    put_hex(number);
    serial_write(" rdi=");
    put_hex(regs->rdi);
    serial_write(" rsi=");
    put_hex(regs->rsi);
    serial_write(" rdx=");
    put_hex(regs->rdx);
    serial_write(" r10=");
    put_hex(regs->r10);
    serial_write(" ->");
}

static void trace_end(uint64_t result)
{
    if (!syscall_trace) {
        return;
    }
    put_hex(result);
    serial_write("\n");
    if (traced_number == SYS_EXIT || traced_number == SYS_EXIT_GROUP) {
        serial_write("TRACE end of program\n");
    }
}

bool user_syscall_trace(void)
{
    return syscall_trace;
}

void user_set_syscall_trace(bool on)
{
    syscall_trace = on;
}

void syscall_handler(struct interrupt_registers *regs, uint64_t number)
{
    syscalls++;
    if (syscall_trace) {
        trace_begin(regs, number);
    }
    syscall_dispatch(regs, number);
    if (syscall_trace) {
        trace_end(regs->rax);
    }
}

static void syscall_dispatch(struct interrupt_registers *regs, uint64_t number)
{
    switch (number) {
    case SYS_WRITE: {
        /* arguments come in the registers the C ABI already uses, which is
         * also what Linux uses, so no shuffling is needed and none is done */
        const char *text = (const char *)(uintptr_t)regs->rsi;
        uint64_t count = regs->rdx;
        uint64_t written = 0U;

        if (text == 0) {
            regs->rax = (uint64_t)-14; /* EFAULT */
            return;
        }
        while (written < count && text[written] != 0) {
            serial_putc(text[written]);
            written++;
        }
        regs->rax = written;
        return;
    }
    case SYS_BRK:
        regs->rax = (uint64_t)(int64_t)user_do_brk(regs->rdi);
        return;
    case SYS_ARCH_PRCTL:
        switch (regs->rdi) {
        case ARCH_SET_FS:
            /* The thread pointer.  MSR_FS_BASE is a per-thread register, so
             * it is set here and restored on the way in and out rather than
             * left pointing wherever the last program put it.  The kernel does
             * its own work in ring 0 and does not use this register, so the
             * value is simply parked in a global and applied at the
             * transitions; writing the MSR from here would also be wrong
             * because the CPU does not save FS across a task switch that does
             * not go through the entry paths. */
            user_fs_base = regs->rsi;
            regs->rax = 0U;
            return;
        case ARCH_GET_FS:
            regs->rax = user_fs_base;
            return;
        default:
            regs->rax = (uint64_t)-38;  /* ENOSYS: not one we implement */
            return;
        }
    case SYS_MMAP:
        user_do_mmap(regs);
        return;
    case SYS_EXIT:
        user_exit_code = regs->rdi;
        regs->rax = 0U;
        serial_write("  user: exited with ");
        put_hex(regs->rdi);
        serial_putc('\n');
        /* The program is finished, so it must not be returned to.
         *
         * Returning here looks harmless and is not: the syscall stub ends in
         * sysretq, which goes back to the instruction after the syscall.  For
         * a program whose last instruction is the exit, that address is one
         * past the end of its text, and the processor faults on whatever
         * instruction bytes happen to be there -- a general protection fault
         * at an address that is not in the program at all, which reads as a
         * paging or a privilege problem and is neither.
         *
         * Linux does not return from exit either: the thread is gone.  So the
         * task is killed here and control does not come back. */
        task_kill_current();
        return;
    default:
        serial_write("  user: unknown syscall ");
        put_hex(number);
        serial_putc('\n');
        regs->rax = (uint64_t)-38; /* ENOSYS */
        return;
    }
}

/* Defined in usertest.S: a program in ring 3, and a stack for it.
 *
 * These are addresses, not values, and they are declared as what they are.
 * Declared as uint64_t and passed by name, C reads the eight bytes stored at
 * each location: for the program that is the first instruction, and for the
 * stack it is uninitialised memory.  Both arrive as enormous numbers, and the
 * ring change fails on the instruction pointer with an error that says nothing
 * about it. */
extern void user_test_entry(void);
extern char user_test_stack_top[];
extern char user_test_image_end[];

#define USER_STACK_BYTES 8192U

/* Load the test program into the user range and run it from there.
 *
 * It cannot run where the linker put it.  That address is in the kernel image,
 * and the kernel image is mapped supervisor-only, so the processor refuses the
 * first instruction fetch with a protection violation -- a page fault that is
 * present on the page and still fails, which reads like a broken segment setup
 * and is not one.  The program and its stack are copied into memory with the
 * user bit set at every level of the walk, which is the only arrangement where
 * ring 3 can reach it. */
static bool user_load_test(uint64_t *entry, uint64_t *stack)
{
    uint64_t image_bytes = (uint64_t)(uintptr_t)user_test_image_end -
                          (uint64_t)(uintptr_t)user_test_entry;
    uint64_t image_pages = (image_bytes + VM_PAGE_BYTES - 1U) / VM_PAGE_BYTES;
    void *image = vm_user_alloc_pages(image_pages * VM_PAGE_BYTES);
    void *stacks;

    if (image == 0) {
        serial_write("  user: no memory for the program: ");
        serial_write(vm_error());
        serial_putc('\n');
        return false;
    }
    stacks = vm_user_alloc_pages(USER_STACK_BYTES);
    if (stacks == 0) {
        serial_write("  user: no memory for the stack: ");
        serial_write(vm_error());
        serial_putc('\n');
        vm_user_free_pages(image, image_pages * VM_PAGE_BYTES);
        return false;
    }
    /* A ring 3 stack grows down, so the first push has to land inside the
     * block rather than just below it.  Sixteen byte alignment is also what
     * iretq wants to see, and an unaligned stack pointer there is a fault. */
    *stack = ((uint64_t)(uintptr_t)stacks + USER_STACK_BYTES) & ~0xFULL;
    __builtin_memcpy(image, (const void *)(uintptr_t)user_test_entry,
                     (size_t)image_bytes);
    *entry = (uint64_t)(uintptr_t)image;
    return true;
}

/* The frame about to be given to iretq, printed before it is used.  A ring
 * change fails on a value in here and the processor reports nothing about
 * which one, so the values are written down while they are still known. */
void user_report_frame(uint64_t entry, uint64_t stack, uint64_t code,
                       uint64_t data, uint64_t stack_selector)
{
    serial_write("  iretq rip=");
    serial_write_decimal(entry);
    serial_write(" rsp=");
    serial_write_decimal(stack);
    serial_write(" cs=");
    serial_write_decimal(code);
    serial_write(" ss=");
    serial_write_decimal(stack_selector);
    serial_write(" data=");
    serial_write_decimal(data);
    serial_write("\n");
}

bool user_run_test(void)
{
    uint64_t entry;
    uint64_t stack;
    int task;

    if (!user_load_test(&entry, &stack)) {
        return false;
    }
    /* A task, not an iretq from here.  Entering ring 3 on this stack and never
     * coming back means the first tick after the program starts has nothing to
     * switch back to, and the guest reboots. */
    task = task_spawn_user(entry, stack);
    if (task < 0) {
        serial_write("  user: no free task slot\n");
        return false;
    }
    serial_write("  user: started as task ");
    serial_write_decimal((uint64_t)task);
    serial_write(" at ");
    serial_write_decimal(entry);
    serial_write(" stack ");
    serial_write_decimal(stack);
    serial_putc('\n');
    return true;
}

uint64_t user_syscall_count(void)
{
    return syscalls;
}

uint64_t user_exit_status(void)
{
    return user_exit_code;
}

bool user_spawn(uint64_t entry, uint64_t stack, uint64_t *code_selector,
                uint64_t *data_selector)
{
    if (user_selectors_ready() == false) {
        return false;
    }
    *code_selector = USER_CODE + 3U;
    *data_selector = USER_DATA + 3U;
    user_report_frame(entry, stack, *code_selector, *data_selector, 0x1B);
    user_enter(entry, stack);
    /* user_enter does not return: it raises the privilege level and the
     * program is somewhere else now.  If it ever does, something is wrong
     * with the segments, so say so rather than carrying on in ring 0. */
    return false;
}
