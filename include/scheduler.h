#ifndef KLYE_SCHEDULER_H
#define KLYE_SCHEDULER_H

#include <stdint.h>

#include "kernel.h"

typedef void (*task_function)(uint64_t argument);

void scheduler_init(void *stack_pointer);
int task_spawn(task_function function, uint64_t argument);
void scheduler_on_interrupt(struct interrupt_registers *registers,
                            struct cpu_frame *frame);
__attribute__((noreturn)) void task_exit(void);

/* Create a task that starts executing in ring 3 at `entry` with `user_stack` as
 * its stack pointer.  The stack must be memory ring 3 can write, which the
 * kernel's own mappings are not; use vm_user_alloc_pages for it.  Returns the
 * task index, or -1. */
int task_spawn_user(uint64_t entry, uint64_t user_stack);

int scheduler_current_task(void);

#endif
