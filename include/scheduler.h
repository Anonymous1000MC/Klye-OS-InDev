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

int scheduler_current_task(void);

#endif
