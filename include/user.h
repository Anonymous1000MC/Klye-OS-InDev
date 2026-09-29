#ifndef KLYE_USER_H
#define KLYE_USER_H

#include <stdbool.h>
#include <stdint.h>

#include "kernel.h"

/* Ring 3, and the syscall that Linux's ABI is built on.
 *
 * A program outside the kernel needs four things this provides: descriptors
 * it can be loaded at, a stack of its own, a way in, and a way to call back.
 * The way in is iretq, which is the only instruction that raises the
 * privilege level.  The way back is the syscall instruction, with the return
 * state kept in two fixed words rather than on the user's stack, because the
 * kernel is using that stack for the duration.
 *
 * One task at a time.  The saved return state is two words in static memory,
 * so a second ring 3 task would overwrite the first's mid syscall.  That is
 * stated rather than left to be discovered, and it is the first thing to
 * change when there is more than one.
 */

/* Whether the user descriptors are in the table yet.  A program cannot be
 * started before this, and attempting to raises a general protection fault
 * with a selector that is not there, which is not a useful first failure. */
bool user_selectors_ready(void);

/* Start a program in ring 3.
 *
 * `entry` is its first instruction and `stack` the top of its stack.  On
 * return from ring 3 it does not come back here.  The selectors the program
 * will be running with come back through the outputs, because a caller that
 * wants to build a context needs them and there is no other way to ask. */
bool user_spawn(uint64_t entry, uint64_t stack, uint64_t *code_selector,
                uint64_t *data_selector);

/* Run the ring 3 test program: a handful of instructions and two syscalls.
 * Returns false only if the user descriptors are not there, since the program
 * itself does not come back. */
bool user_run_test(void);

/* How many syscalls have been serviced, and what a program last exited with.
 * For the shell command that runs the test, and for nothing else. */
uint64_t user_syscall_count(void);
uint64_t user_exit_status(void);

/* The register state a syscall handler is given.  The kernel's own struct is
 * used rather than a private one, so that the syscall stub and the interrupt
 * stub can share a dispatcher.  The layout is the kernel's internal business
 * and is not part of any interface a user program sees. */
void syscall_handler(struct interrupt_registers *registers, uint64_t number);

/* Print a marker over the serial port, for tracing the ring 3 path.  A triple
 * fault resets the processor before anything can report itself, so the only
 * way to know how far execution got is to have said so on the way in. */
void serial_marker(const char *text);

/* Defined in ring3.S. */
void user_enter(uint64_t entry, uint64_t stack);
void syscall_entry(void);

#endif

/* Tell brk which region it may hand out, once the program's address space
 * exists.  `start` is the first address of the heap and `end` the first address
 * past it. */
void user_set_heap(uint64_t start, uint64_t end);

/* Print every syscall with its arguments and result.  Off by default; the
 * `usertest` command with no program toggles it. */
bool user_syscall_trace(void);
void user_set_syscall_trace(bool on);
