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
#include "vfs.h"
#include "fdtable.h"
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
#define SYS_SET_TID_ADDRESS 218
#define SYS_GETTID 186

/* The user half, and low enough to miss anything the kernel maps.  These
 * match the range elf.c will load a program into, so a pointer a program
 * legitimately holds always passes. */
#define USER_POINTER_LOW 0x100000ULL
#define USER_POINTER_HIGH 0x00007FFFFFFFF000ULL

/* A user pointer, checked.  A syscall that dereferences what the program
 * passed has to know the pointer is in the user half before it reads it, or a
 * program with a bad pointer takes the kernel down with it: the read faults in
 * ring 0, where there is nothing to catch it, and the whole machine stops.
 *
 * A vector array is a handful of entries, so the check is on the whole span
 * at once rather than per entry. */
static bool user_pointer_ok(const void *pointer, uint64_t length)
{
    uint64_t start = (uint64_t)(uintptr_t)pointer;

    if (length == 0U) {
        return start < USER_POINTER_HIGH;
    }
    if (start < USER_POINTER_LOW || start >= USER_POINTER_HIGH) {
        return false;
    }
    if (length > USER_POINTER_HIGH - start) {
        return false;
    }
    return true;
}

/* The vector entry a readv/writev is given.  Linux puts the address first and
 * the length second, and the order is load-bearing: a program builds these
 * from its own headers and the kernel reads the bytes, so getting the fields
 * the wrong way round does not fail -- it reads the length as an address and
 * the address as a length, and then faults in ring 0 on what it believes is
 * text. */
struct iovec {
    uint64_t iov_base;
    uint64_t iov_len;
};

/* File syscalls.  musl's stdio uses open, read, readv, writev, lseek, close
 * and ioctl, and touches fstat and fcntl only for particular open modes.  The
 * two that are easy to miss are readv: __stdio_read issues a two-entry vector
 * read for every buffered read, falling back to plain read only when the
 * caller's buffer is empty.  And ioctl: __fdopen asks for the window size on
 * any FILE that is not read-only, to know how wide its output may be. */
#define SYS_READ 0
#define SYS_CLOSE 3
#define SYS_LSEEK 8
#define SYS_OPEN 2
#define SYS_IOCTL 16
#define SYS_READV 19
#define SYS_WRITEV 20
#define SYS_OPENAT 257
#define SYS_FSTAT 5
#define SYS_RT_SIGPROCMASK 14

/* open(2) flags, x86-64 Linux values.  Only the ones that change behaviour
 * are named; the rest are accepted and ignored, because refusing an open a
 * program had every right to make is a worse failure than serving it. */
#define O_ACCMODE 0003
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0100
#define O_TRUNC 01000
#define O_APPEND 02000

/* The ioctl request musl sends for a window size, and the answer it expects
 * from something that is not a terminal.  Returning ENOTTY is the honest
 * reply: this is not a tty, and the library carries on with a default width. */
#define TIOCGWINSZ 0x5413
#define ENOTTY 25
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

/* Where musl asked to have its thread id written.  Recorded, not honoured yet:
 * the write itself needs a place to put the value that survives a context
 * switch, which is the task structure, and that is a separate piece of work.
 * Recording it means the tid the program reads is at least the right one. */
static uint64_t user_tid_address;
static uint64_t user_tid;
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
extern void user_canary_entry(void);
extern void user_test_entry(void);

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
    case SYS_RT_SIGPROCMASK:
        /* Not implemented as a mask -- there is nothing to deliver yet -- but
         * it has to answer rather than spin.  musl calls this from abort, so
         * a program that hits any fatal condition and then retries gets an
         * unbounded loop of sigprocmask that looks like a signal delivery
         * fault and is not one.  Succeeding with no change is the least
         * surprising answer until there is a signal mask to change. */
        regs->rax = 0U;
        return;
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
        /* Counted, not scanned, for the same reason as writev: a write is a
         * length, and stopping at a NUL reports a short write for a request
         * that in fact succeeded. */
        for (written = 0U; written < count; ++written) {
            serial_putc(text[written]);
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
    case SYS_OPEN:
    case SYS_OPENAT: {
        /* open(2) is a path and flags.  openat(2) is the same with a
         * directory descriptor first, and the only one that can be relative
         * to something other than the working directory -- of which there is
         * one, so AT_FDCWD is the whole of it today.
         *
         * The C library calls open first and openat second, on the theory
         * that a kernel without open is old.  Both are cheap and both are
         * implemented, because a program that only ever gets -ENOSYS from one
         * of them has no way to open anything. */
        const char *path;
        uint64_t flags;
        int access;
        bool creating;
        int node;
        int fd;

        if (number == SYS_OPEN) {
            path = (const char *)(uintptr_t)regs->rdi;
            flags = regs->rsi;
        } else {
            path = (const char *)(uintptr_t)regs->rsi;
            flags = regs->rdx;
        }
        if (path == 0) {
            regs->rax = (uint64_t)-14; /* EFAULT */
            return;
        }
        access = (int)(flags & O_ACCMODE);
        creating = (flags & O_CREAT) != 0U;

        node = vfs_open(path);
        if (node < 0) {
            if (!creating) {
                regs->rax = (uint64_t)-2; /* ENOENT */
                return;
            }
            /* Creation is served if the VFS can do it, and refused clearly if
             * it cannot.  A create that silently does nothing would leave the
             * program writing into a descriptor whose contents go nowhere. */
            if (vfs_touch(path) < 0) {
                regs->rax = (uint64_t)-30; /* EROFS: read-only image */
                return;
            }
            node = vfs_open(path);
            if (node < 0) {
                regs->rax = (uint64_t)-2;
                return;
            }
        }
        fd = fd_alloc_file(node, access != O_WRONLY, access != O_RDONLY);
        if (fd < 0) {
            regs->rax = (uint64_t)-24; /* EMFILE */
            return;
        }
        regs->rax = (uint64_t)(int64_t)fd;
        return;
    }
    case SYS_WRITEV: {
        /* musl's buffered write is a vector write, and it is the only way
         * printf reaches the terminal.  Returning ENOSYS here does not fail
         * the call cleanly -- musl treats it as a fatal write error, reports
         * it through the same path, and retries forever.
         *
         * The count in rdx is how many iovecs there are, and it is bounded by
         * what the caller claims.  A count of all-ones would walk the vector
         * array off the end of the mapping, so it is clamped: no writev needs
         * more entries than there are bytes it could describe. */
        const struct iovec *iov = (const struct iovec *)(uintptr_t)regs->rsi;
        uint64_t iovcnt = regs->rdx;
        uint64_t total = 0U;

        if (iov == 0 || iovcnt == 0U) {
            regs->rax = 0U;
            return;
        }
        if (!fd_is_writable((int)regs->rdi)) {
            regs->rax = (uint64_t)-9; /* EBADF */
            return;
        }
        if (iovcnt > 1024U) {
            iovcnt = 1024U;
        }
        /* The vector array and the bytes it points at are both the program's
         * memory, and both have to be checked before either is read.  A bad
         * one is the program's mistake, not the kernel's, and EFAULT is how
         * a program is told so. */
        if (!user_pointer_ok(iov, iovcnt * (uint64_t)sizeof(struct iovec))) {
            regs->rax = (uint64_t)-14; /* EFAULT */
            return;
        }
        for (uint64_t i = 0; i < iovcnt; ++i) {
            const char *text = (const char *)(uintptr_t)iov[i].iov_base;
            uint64_t length = iov[i].iov_len;

            if (text == 0 || length == 0U) {
                continue;
            }
            /* The bytes are counted, not scanned.  A writev is a length, not a
             * string: stopping at the first NUL means writing less than the
             * caller asked for and telling it so, and a C library that
             * believes a short write is a retryable condition will retry with
             * buffer state that no longer matches the file.  That is how one
             * correct call turns into an unbounded loop of EFAULT.
             *
             * The one exception is the console, which is a terminal and not a
             * byte sink -- there is nothing after a NUL to write, and counting
             * a run of padding as written would be a different lie. */
            if (!user_pointer_ok(text, length)) {
                /* Stop, but report what was already written.
                 *
                 * Returning EFAULT here throws away the bytes that went out
                 * with the earlier entries and tells the caller nothing
                 * succeeded.  A library that trusts the error and retries with
                 * the same vector writes the earlier entries again and again,
                 * and a library that trusts it as fatal stops mid-line.  The
                 * truthful answer is the number of bytes that reached the
                 * terminal, which is a short write and which every caller in
                 * this ABI already knows how to handle. */
                if (total > 0U) {
                    /* Some of the vector went out.  Report the count and stop:
                     * a caller that trusts the number resumes where it left
                     * off, and one that trusts an error retries the whole
                     * vector and duplicates what already went out. */
                    break;
                }
                regs->rax = (uint64_t)-14; /* EFAULT: nothing could be written */
                return;
            }
            for (uint64_t at = 0; at < length; ++at) {
                serial_putc(text[at]);
                total++;
            }
        }
        regs->rax = total;
        return;
    }
    case SYS_READ: {
        char *out = (char *)(uintptr_t)regs->rsi;
        uint32_t count = (uint32_t)regs->rdx;
        int node;
        int got;

        if (!fd_is_readable((int)regs->rdi)) {
            regs->rax = (uint64_t)-9; /* EBADF */
            return;
        }
        if (out == 0 || count == 0U) {
            regs->rax = 0U;
            return;
        }
        node = fd_node((int)regs->rdi);
        if (node < 0) {
            /* A read from the console has no data to give and no way to wait
             * for any.  Zero is what a non-blocking read at end of input
             * returns, and an error here would make every program that polls
             * its own input think the descriptor is broken. */
            regs->rax = 0U;
            return;
        }
        got = vfs_read_at_node(node, out, fd_cursor((int)regs->rdi), count);
        if (got < 0) {
            regs->rax = (uint64_t)-5; /* EIO */
            return;
        }
        fd_advance((int)regs->rdi, (uint32_t)got);
        regs->rax = (uint64_t)(int64_t)got;
        return;
    }
    case SYS_READV: {
        /* A vector read, and not an exotic one: musl's buffered read issues
         * this for every read, with one iovec for the caller's buffer and
         * room in the second for anything left over.  Returning ENOSYS here
         * does not fail the call cleanly -- it strands the second entry and
         * the library's buffer bookkeeping stops matching the file, which
         * looks later like a filesystem bug rather than a missing syscall. */
        const struct iovec *iov = (const struct iovec *)(uintptr_t)regs->rsi;
        uint64_t iovcnt = regs->rdx;
        uint32_t total = 0U;
        int node;

        if (!fd_is_readable((int)regs->rdi)) {
            regs->rax = (uint64_t)-9;
            return;
        }
        node = fd_node((int)regs->rdi);
        if (iov == 0 || iovcnt == 0U) {
            regs->rax = 0U;
            return;
        }
        for (uint64_t i = 0; i < iovcnt; ++i) {
            char *base = (char *)(uintptr_t)iov[i].iov_base;
            uint32_t length = (uint32_t)iov[i].iov_len;
            int got;

            if (base == 0 || length == 0U) {
                continue;
            }
            if (node < 0) {
                break;
            }
            got = vfs_read_at_node(node, base, fd_cursor((int)regs->rdi), length);
            if (got <= 0) {
                break;
            }
            fd_advance((int)regs->rdi, (uint32_t)got);
            total += (uint32_t)got;
            if ((uint32_t)got < length) {
                break;  /* end of file */
            }
        }
        regs->rax = (uint64_t)total;
        return;
    }
    case SYS_LSEEK: {
        /* Three whence values, and only one of them is the offset the caller
         * means: SEEK_SET from the start, SEEK_CUR from the cursor, SEEK_END
         * from the size.  A descriptor that is not a file cannot seek, and
         * says so rather than pretending the cursor is a position. */
        int64_t offset = (int64_t)regs->rsi;
        int whence = (int)regs->rdx;
        int64_t target;
        uint32_t size;
        int node;

        if (fd_lookup((int)regs->rdi) == 0) {
            regs->rax = (uint64_t)-9;
            return;
        }
        if (!fd_seekable((int)regs->rdi)) {
            regs->rax = (uint64_t)-29; /* ESPIPE: not seekable */
            return;
        }
        node = fd_node((int)regs->rdi);
        size = (int64_t)vfs_size(node);
        if (whence == 0) {
            target = offset;
        } else if (whence == 1) {
            target = (int64_t)fd_cursor((int)regs->rdi) + offset;
        } else if (whence == 2) {
            target = (int64_t)size + offset;
        } else {
            regs->rax = (uint64_t)-22; /* EINVAL */
            return;
        }
        if (target < 0) {
            regs->rax = (uint64_t)-22;
            return;
        }
        /* Seeking past the end is allowed and is how a program makes a hole
         * it writes into.  It is not an error until a read happens there. */
        fd_advance((int)regs->rdi, (uint32_t)target - (uint32_t)fd_cursor((int)regs->rdi));
        regs->rax = (uint64_t)target;
        return;
    }
    case SYS_CLOSE: {
        regs->rax = (uint64_t)(int64_t)fd_close((int)regs->rdi);
        return;
    }
    case SYS_IOCTL: {
        /* Only a window-size query exists, and nothing here has a terminal.
         *
         * ENOTTY is the honest answer, but it turns out musl's stdio treats a
         * failed width query on stdout as a reason to abort during its own
         * initialisation -- before main -- and then spins in abort calling
         * sigprocmask.  So a truthful errno was killing the program at a point
         * where the library had no way to carry on.
         *
         * Succeed with a plausible 80x24 and let the library format to that.
         * Wrong for a real terminal, harmless everywhere else, and it is a
         * documented limitation rather than a silent lie: nothing in this
         * kernel has a window size to report. */
        uint16_t *out = (uint16_t *)(uintptr_t)regs->rdx;

        if (fd_lookup((int)regs->rdi) == 0) {
            regs->rax = (uint64_t)-9;
            return;
        }
        if (regs->rsi == (uint64_t)TIOCGWINSZ && out != 0) {
            out[0] = 24U; /* rows */
            out[1] = 80U; /* columns */
            out[2] = 0U;
            out[3] = 0U;
            regs->rax = 0U;
            return;
        }
        regs->rax = (uint64_t)-25; /* ENOTTY */
        return;
    }
    case SYS_SET_TID_ADDRESS:
        /* musl calls this from __init_tp, during TLS setup, before main and
         * with no threading involved.  It hands back the address the thread
         * wants to be able to have its tid written to, and the kernel is
         * supposed to return the calling thread's id.
         *
         * Nothing on the startup path looks at the answer, which is why a
         * program reaches main even when this returns -ENOSYS -- and then
         * stores -38 in its thread structure and uses that as a thread and
         * signal identity for the rest of its life.  A silent wrong answer is
         * worse than a missing one. */
        user_tid_address = regs->rdi;
        user_tid = (uint64_t)(scheduler_current_task() + 1);
        regs->rax = user_tid;
        return;
    case SYS_GETTID:
        /* Cheap, and it means the tid this kernel hands out is at least
         * reachable: a program that asks twice gets the same answer, which is
         * the property thread identity exists to provide. */
        regs->rax = (uint64_t)(int64_t)(scheduler_current_task() + 1);
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
        /* Only under strace.  An unimplemented syscall is an ordinary thing
         * for a program to do, not an event worth a line of serial, and a
         * print here sits between the frame and the checks that look at it. */
        if (syscall_trace)
            serial_write("  user: unknown syscall ");
        if (syscall_trace) {
            put_hex(number);
            serial_putc('\n');
        }
        /* What the frame actually holds for the four registers the canary
         * says come back wrong.  Printed from the kernel rather than from the
         * test program: put_hex already works here, and the test has to do
         * this in assembly against a blob that gets copied, which is a poor
         * place to be debugging.  The value at entry is the one the program
         * set, so if it matches the canary's expected value the damage is
         * happening on the way back out and not on the way in. */
        if (number == 999U) {
            serial_write("  frame rdi=");
            put_hex(regs->rdi);
            serial_write(" rsi=");
            put_hex(regs->rsi);
            serial_write(" rdx=");
            put_hex(regs->rdx);
            serial_write(" r8=");
            put_hex(regs->r8);
            serial_write(" rbx=");
            put_hex(regs->rbx);
            serial_putc('\n');
        }
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

/* Run the syscall register canary: every register set to a distinct value,
 * one unimplemented syscall, then each compared.  Anything that comes back
 * changed is printed with its name. */
bool user_run_canary(void)
{
    uint64_t entry;
    uint64_t stack;
    int task;

    if (!user_load_test(&entry, &stack)) {
        return false;
    }
    /* user_load_test copies the blob and hands back its base, so the canary is
     * reached by adding its offset within the blob rather than by naming it --
     * the copy is what runs, not the original. */
    entry += (uint64_t)(uintptr_t)&user_canary_entry -
             (uint64_t)(uintptr_t)&user_test_entry;
    task = task_spawn_user(entry, stack);
    if (task < 0) {
        return false;
    }
    serial_write("  canary: syscall 999, checking every register\n");
    return true;
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
