#include <stdbool.h>
#include <stdint.h>

#include "kernel.h"
#include "scheduler.h"

#define TASK_LIMIT 8
#define TASK_STACK_SIZE 16384

struct task {
    struct interrupt_registers registers;
    struct cpu_frame frame;
    task_function function;
    uint64_t argument;
    bool ready;
};

extern __attribute__((noreturn)) void task_context_restore(
    struct interrupt_registers *registers, struct cpu_frame *frame);
extern void task_trampoline_entry(void);

_Static_assert(sizeof(struct cpu_frame) == 48, "invalid CPU frame layout");
_Static_assert(sizeof(struct interrupt_registers) == 120,
               "invalid interrupt register layout");

static struct task tasks[TASK_LIMIT];
static uint8_t task_stacks[TASK_LIMIT][TASK_STACK_SIZE]
    __attribute__((aligned(16)));
static int current_task_index = -1;

int scheduler_current_task(void)
{
    return current_task_index;
}

static int next_ready_task(int start)
{
    for (uint32_t offset = 1; offset <= TASK_LIMIT; ++offset) {
        int index = (start + (int)offset) % TASK_LIMIT;
        if (tasks[index].ready) {
            return index;
        }
    }
    return -1;
}

void scheduler_init(void *stack_pointer)
{
    tasks[0].registers = (struct interrupt_registers){0};
    tasks[0].frame = (struct cpu_frame){0};
    tasks[0].frame.rsp = (uint64_t)(uintptr_t)stack_pointer;
    tasks[0].function = 0;
    tasks[0].argument = 0;
    tasks[0].ready = true;
    current_task_index = 0;
}

int task_spawn(task_function function, uint64_t argument)
{
    if (function == 0) {
        return -1;
    }

    for (int index = 1; index < TASK_LIMIT; ++index) {
        struct task *task = &tasks[index];
        uint64_t stack_top = (uint64_t)(uintptr_t)&task_stacks[index][TASK_STACK_SIZE];

        if (task->ready) {
            continue;
        }

        task->registers = (struct interrupt_registers){0};
        task->frame = (struct cpu_frame){0};
        task->frame.rip = (uint64_t)(uintptr_t)task_trampoline_entry;
        task->frame.cs = 0x08;
        task->frame.rflags = 0x202;
        task->frame.rsp = stack_top - 8;
        task->frame.ss = 0x10;
        *(uint64_t *)(uintptr_t)(stack_top - 8) = (uint64_t)index;
        task->function = function;
        task->argument = argument;
        task->ready = true;
        return index;
    }

    return -1;
}

void scheduler_on_interrupt(struct interrupt_registers *registers,
                            struct cpu_frame *frame)
{
    struct task *current;
    int next;

    if (current_task_index < 0) {
        return;
    }

    current = &tasks[current_task_index];
    current->registers = *registers;
    current->frame = *frame;
    current->frame.rflags |= 1U << 9;
    current->ready = true;

    next = next_ready_task(current_task_index);
    if (next < 0 || next == current_task_index) {
        return;
    }

    current_task_index = next;
    task_context_restore(&tasks[next].registers, &tasks[next].frame);
}

void task_trampoline(uint64_t task_index)
{
    struct task *task;

    if (task_index >= TASK_LIMIT) {
        panic("invalid task entry");
    }

    task = &tasks[task_index];
    task->function(task->argument);
    task_exit();
}

__attribute__((noreturn)) void task_exit(void)
{
    int next;

    __asm__ volatile("cli");
    if (current_task_index < 0) {
        panic("task exit without scheduler");
    }

    tasks[current_task_index].ready = false;
    next = next_ready_task(current_task_index);
    if (next < 0) {
        panic("no runnable task");
    }

    current_task_index = next;
    task_context_restore(&tasks[next].registers, &tasks[next].frame);
}
