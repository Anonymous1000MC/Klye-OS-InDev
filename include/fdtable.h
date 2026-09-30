/* File descriptors for user programs.  See fdtable.c for why this exists. */
#ifndef INCLUDE_FDTABLE_H
#define INCLUDE_FDTABLE_H

#include <stdbool.h>
#include <stdint.h>

void fd_table_init(void);
int fd_alloc_file(int node, bool readable, bool writable);
int fd_close(int fd);

/* Resolve a descriptor.  Returns 0 for a number that is not open, which is
 * the only way a caller can tell EBADF from a real file.  Exposed because the
 * syscalls in user.c are where a descriptor is actually used, and a table that
 * only its own file can see is a table nothing can read through. */
struct fd_entry;
const struct fd_entry *fd_lookup(int fd);
bool fd_is_readable(int fd);
bool fd_is_writable(int fd);
int fd_cursor(int fd);
void fd_advance(int fd, uint32_t bytes);
int fd_node(int fd);
bool fd_seekable(int fd);

#endif
