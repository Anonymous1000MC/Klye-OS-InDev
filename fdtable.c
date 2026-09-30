#include "fdtable.h"

#include <stdbool.h>
#include <stdint.h>

/* File descriptors for user programs.
 *
 * Linux hands a program an integer and makes it responsible for passing it
 * back on every later call.  Nothing about the file travels with the call, so
 * the kernel has to keep the mapping from that integer to whatever the file
 * actually is.  This is that mapping, and it is the first thing a file syscall
 * needs: open(2) returns an fd, and read(2) takes an fd, so there is no way to
 * build read without this first.
 *
 * Deliberately small.  The VFS underneath is path-based and already supports
 * read-at-an-offset, so a descriptor is a node index plus a cursor, not a
 * buffer.  Buffered I/O belongs to the C library, which does it in user space;
 * a kernel fd table that also buffered would mean two buffers and two answers
 * to "where in the file are we". */

/* Enough for a program with a handful of files open.  Linux allows far more,
 * but nothing that runs here does, and a fixed table is a fixed amount of
 * kernel memory that cannot be exhausted by a runaway program. */
#define FD_MAX 64

enum fd_kind {
    FD_FREE = 0,
    FD_CONSOLE,  /* stdin, stdout, stderr: not a file, no cursor */
    FD_FILE      /* a node in the VFS, with a read/write cursor */
};

struct fd_entry {
    enum fd_kind kind;
    int node;        /* VFS node index, for FD_FILE */
    uint32_t cursor; /* read and write position, for FD_FILE */
    bool readable;
    bool writable;
};

static struct fd_entry fd_table[FD_MAX];
static int fd_first_free(void)
{
    for (int i = 0; i < FD_MAX; ++i) {
        if (fd_table[i].kind == FD_FREE) {
            return i;
        }
    }
    return -1;
}

/* Called once, before any user program runs.  The three standard descriptors
 * have to exist before main: a C library writes its own diagnostics to stderr
 * during startup, and a program that opens a file and gets 1 back instead of 3
 * has no way to report that. */
void fd_table_init(void)
{
    for (int i = 0; i < FD_MAX; ++i) {
        fd_table[i].kind = FD_FREE;
        fd_table[i].node = -1;
        fd_table[i].cursor = 0U;
    }
    /* 0 read-only, 1 and 2 writable.  Linux also allows a file to be opened
     * read-write on descriptor 0, and does not care that a console is not
     * seekable; neither does anything that runs here. */
    fd_table[0].kind = FD_CONSOLE;
    fd_table[0].readable = true;
    fd_table[0].writable = false;
    fd_table[1].kind = FD_CONSOLE;
    fd_table[1].readable = false;
    fd_table[1].writable = true;
    fd_table[2].kind = FD_CONSOLE;
    fd_table[2].readable = false;
    fd_table[2].writable = true;
}

static struct fd_entry *fd_get(int fd)
{
    if (fd < 0 || fd >= FD_MAX || fd_table[fd].kind == FD_FREE) {
        return 0;
    }
    return &fd_table[fd];
}

/* The accessors below are the interface user.c uses, rather than handing it
 * the table and a struct definition: the syscalls want to know whether a
 * number is open, whether it can be read, and where its cursor is, and those
 * are questions rather than fields. */
const struct fd_entry *fd_lookup(int fd)
{
    return fd_get(fd);
}

bool fd_is_readable(int fd)
{
    const struct fd_entry *entry = fd_get(fd);
    return entry != 0 && entry->readable;
}

bool fd_is_writable(int fd)
{
    const struct fd_entry *entry = fd_get(fd);
    return entry != 0 && entry->writable;
}

bool fd_seekable(int fd)
{
    const struct fd_entry *entry = fd_get(fd);
    return entry != 0 && entry->kind == FD_FILE;
}

int fd_node(int fd)
{
    const struct fd_entry *entry = fd_get(fd);
    return entry == 0 ? -1 : entry->node;
}

int fd_cursor(int fd)
{
    const struct fd_entry *entry = fd_get(fd);
    return entry == 0 ? 0 : (int)entry->cursor;
}

void fd_advance(int fd, uint32_t bytes)
{
    struct fd_entry *entry = fd_get(fd);
    if (entry != 0) {
        entry->cursor += bytes;
    }
}

int fd_alloc_file(int node, bool readable, bool writable)
{
    int fd = fd_first_free();

    if (fd < 0 || node < 0) {
        return -1;
    }
    fd_table[fd].kind = FD_FILE;
    fd_table[fd].node = node;
    fd_table[fd].cursor = 0U;
    fd_table[fd].readable = readable;
    fd_table[fd].writable = writable;
    return fd;
}

int fd_close(int fd)
{
    struct fd_entry *entry = fd_get(fd);

    if (entry == 0) {
        return -9; /* EBADF */
    }
    entry->kind = FD_FREE;
    entry->node = -1;
    entry->cursor = 0U;
    return 0;
}
