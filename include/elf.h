#ifndef KLYE_ELF_H
#define KLYE_ELF_H

#include <stdbool.h>
#include <stdint.h>

/* Loading an ELF64 executable and running it in ring 3.
 *
 * This is the whole of what "Linux binary support" needs before a real program
 * will start, and it is deliberately small: read the headers, map the segments,
 * move a position independent binary to where it was linked, hand it a stack
 * and the auxiliary vector libc reads at startup, and let the program call
 * back through the syscall path that already works.
 *
 * What is NOT here is anything a C library needs once it is running: the
 * syscall numbers themselves, signals, a futex, TLS.  A freestanding program
 * that only issues syscalls runs from this.  A musl or dietlibc program does
 * not, and cannot until those exist -- see TODO.md.
 */

/* Where a position independent executable is loaded.
 *
 * It has to be inside the user mapping window, not merely inside the address
 * space: the window is the one range of the address space whose page tables
 * carry the user bit at every level, and a program loaded anywhere else is
 * mapped supervisor-only, so ring 3 is refused its own first instruction.  A
 * low address like 0x400000, which is what a program expects, falls in the
 * identity map and is exactly such an address.
 *
 * The window is the second top level slot, 512 GB wide.  The loader checks
 * this against vm_user_map_at rather than assuming it, so if the window ever
 * moves the error says so instead of the program faulting. */
#define USER_ELF_LOAD_BIAS 0x10000000000ULL

/* Stack for a loaded program.  64 KiB: the ring 3 test needs almost nothing,
 * and a real C library will want more than this, but there is no growable
 * stack yet, so a program that needs more fails inside its own code rather
 * than here. */
#define ELF_STACK_BYTES (64U * 1024U)

/* The heap a program may grow into, reserved above its stack.
 *
 * brk moves a break inside a region the program owns; it does not map more on
 * demand.  Reserving it up front costs address space and nothing else, and it
 * means a program cannot grow its heap over a page another program is using. */
/* Gap between the end of a program's image and where the heap starts, and
 * between the heap and where the stack region begins.  The image and the stack
 * are far too close together for a 4 MiB heap between them: there are about
 * 128 KiB, and a heap that does not fit lands below the image entirely. */
#define ELF_MMAP_GAP_BYTES (256U * 1024U)
#define ELF_STACK_GAP_BYTES (64U * 1024U)

/* The window mmap allocates from, reserved below the brk heap.  It is not the
 * heap: sharing one address between the two is what made malloc hand out the
 * same page twice. */
#define ELF_MMAP_BYTES (8U * 1024U * 1024U)

#define ELF_HEAP_BYTES (4U * 1024U * 1024U)

/* Load and start the ELF executable at `path`.
 *
 * Returns false and leaves a reason in elf_error() when anything is missing,
 * malformed, or asks for memory that cannot be had. */
bool elf_run(const char *path);

/* Why the last elf_run failed, or "" if it did not fail. */
const char *elf_error(void);

#endif
