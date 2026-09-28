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
#define SYS_WRITE 1
#define SYS_EXIT  60

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

void syscall_handler(struct interrupt_registers *regs, uint64_t number)
{
    syscalls++;
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
    case SYS_EXIT:
        user_exit_code = regs->rdi;
        regs->rax = 0U;
        serial_write("  user: exited with ");
        put_hex(regs->rdi);
        serial_putc('\n');
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
    uint64_t code;
    uint64_t data;
    uint64_t entry;
    uint64_t stack;

    if (!user_load_test(&entry, &stack)) {
        return false;
    }
    serial_write("  user: loaded at ");
    serial_write_decimal(entry);
    serial_write(" stack ");
    serial_write_decimal(stack);
    serial_putc('\n');
    return user_spawn(entry, stack, &code, &data);
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
