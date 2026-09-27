#include <stdbool.h>
#include <stdint.h>

#include "heap.h"
#include "kernel.h"
#include "scheduler.h"

#define TASK_LIMIT 6
#define TASK_STACK_SIZE 131072

/* 512 byte FXSAVE area: control word, MXCSR, the x87 stack and XMM0-15.
 * Required because libc and the Lua runtime use SSE for double arithmetic, so
 * a task switch in the middle of a floating point expression must not lose
 * the register contents. */
#define TASK_FPU_BYTES 512

struct task {
    struct interrupt_registers registers;
    struct cpu_frame frame;
    task_function function;
    uint64_t argument;
    uint8_t fpu[TASK_FPU_BYTES] __attribute__((aligned(16)));
    bool ready;
};

extern void task_fpu_save(uint8_t *area);
extern void task_fpu_restore(const uint8_t *area);

extern __attribute__((noreturn)) void task_context_restore(
    struct interrupt_registers *registers, struct cpu_frame *frame);
extern void task_trampoline_entry(void);

_Static_assert(sizeof(struct cpu_frame) == 48, "invalid CPU frame layout");
_Static_assert(sizeof(struct interrupt_registers) == 120,
               "invalid interrupt register layout");

/* A fresh task must not inherit an all-zero FPU area: FXSAVE image offset 0
 * holds the x87 control word and offset 8 holds MXCSR, so zeros would unmask
 * every x87 and SSE exception.  The first floating point operation a task
 * performs - in Lua, dividing to work out dt - would then raise the inexact
 * condition and trap, killing the task with no fault report.
 *
 * 0x037F is the x87 control word a C program starts with (every exception
 * masked, extended precision, round to nearest).  0x1F80 is the matching
 * MXCSR: every SSE exception masked, double precision, no flush to zero. */
static void fpu_reset(uint8_t *area)
{
    __builtin_memset(area, 0, TASK_FPU_BYTES);
    area[0] = 0x7F;
    area[1] = 0x03;
    area[8] = 0x80;
    area[9] = 0x1F;
}

static struct task tasks[TASK_LIMIT];
/* Stacks come from the frame heap rather than BSS.  Six 128 KiB stacks was
 * 768 KiB of static data, which is a lot of linker budget to spend on memory
 * the kernel is not using until a task actually needs it. */
static uint8_t *task_stacks[TASK_LIMIT];
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
    fpu_reset(tasks[0].fpu);
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
        uint64_t stack_top;

        if (task->ready) {
            continue;
        }
        if (task_stacks[index] == 0) {
            void *pages = heap_alloc_pages(TASK_STACK_SIZE);

            if (pages == 0) {
                panic("task: no memory for a stack");
            }
            task_stacks[index] = (uint8_t *)(uintptr_t)pages;
        }
        stack_top = (uint64_t)(uintptr_t)(task_stacks[index] + TASK_STACK_SIZE);

        task->registers = (struct interrupt_registers){0};
        task->frame = (struct cpu_frame){0};
        fpu_reset(task->fpu);
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
    task_fpu_save(current->fpu);
    current->registers = *registers;
    current->frame = *frame;
    current->frame.rflags |= 1U << 9;
    current->ready = true;

    next = next_ready_task(current_task_index);
    if (next < 0 || next == current_task_index) {
        return;
    }

    current_task_index = next;
    task_fpu_restore(tasks[next].fpu);
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
