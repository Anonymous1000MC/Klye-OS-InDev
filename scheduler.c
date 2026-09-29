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

/* A task that runs in ring 3.
 *
 * This is what the ring 3 test cannot do for itself.  Entering ring 3 from a
 * shell command runs the program on the shell's own saved state, and there is
 * no way back: the tick that arrives a millisecond later switches to another
 * task, and when this one is next resumed its context describes a program that
 * has already been somewhere else.  What the guest sees is a reboot, or a hang
 * where the reboot would otherwise be -- both are the same defect, and neither
 * is fixed by anything inside the program.
 *
 * So a user program gets a task of its own, with a stack it owns, and the
 * scheduler resumes it like any other.  Two things have to be right for that to
 * work.  The frame is an iretq frame, so the code and stack selectors carry the
 * ring in their low two bits, and they are only ever handed back to iretq.  And
 * the stack has to come from the user range rather than the kernel one: the
 * pages the kernel uses are supervisor-only, and a ring 3 push onto one of them
 * is a protection violation, so a stack allocated here could not be written.
 */
struct user_task_request {
    uint64_t entry;
    uint64_t stack;
};

static struct user_task_request user_requests[TASK_LIMIT];

int task_spawn_user(uint64_t entry, uint64_t user_stack)
{
    if (user_stack == 0) {
        return -1;
    }
    for (int index = 1; index < TASK_LIMIT; ++index) {
        if (tasks[index].ready) {
            continue;
        }
        /* The kernel stack is what a tick arriving from ring 3 is given, and it
         * has to be a real one.  The program's own stack is the user range
         * allocation the caller already made. */
        if (task_stacks[index] == 0) {
            void *pages = heap_alloc_pages(TASK_STACK_SIZE);

            if (pages == 0) {
                panic("user task: no memory for a kernel stack");
            }
            task_stacks[index] = (uint8_t *)(uintptr_t)pages;
        }
        user_requests[index].entry = entry;
        user_requests[index].stack = user_stack;
        return task_spawn(0, (uint64_t)index);
    }
    return -1;
}

int task_spawn(task_function function, uint64_t argument)
{
    if (function == 0 && argument == 0) {
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
        if (function == 0) {
            /* a ring 3 task: enter the program directly, no trampoline */
            task->frame.rip = user_requests[index].entry;
            task->frame.cs = 0x23;   /* user code, ring 3 */
            task->frame.rflags = 0x202;
            task->frame.rsp = user_requests[index].stack;
            task->frame.ss = 0x1B;    /* user data, ring 3 */
            task->function = 0;
            task->argument = 0;
        } else {
            task->frame.rip = (uint64_t)(uintptr_t)task_trampoline_entry;
            task->frame.cs = 0x08;
            task->frame.rflags = 0x202;
            task->frame.rsp = stack_top - 8;
            task->frame.ss = 0x10;
            *(uint64_t *)(uintptr_t)(stack_top - 8) = (uint64_t)index;
            task->function = function;
            task->argument = argument;
        }
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

/* End the running task from inside a syscall.
 *
 * Same effect as task_exit and for the same reason, but reachable from a
 * syscall handler: the stub is going to sysretq back to user mode when the
 * handler returns, and a program that has just called exit has no instruction
 * to come back to.  task_exit is for the trampoline, which is called rather
 * than returned through, and its "the function returned" comment does not
 * describe this case, so this is spelled out separately.
 *
 * The task's pages are deliberately not released.  The mappings belong to the
 * program and something else may still be holding a pointer into them -- the
 * shell that reported the exit code, for one -- and a use after free here would
 * be a fault with no way to tell it from any other. */
__attribute__((noreturn)) void task_kill_current(void)
{
    int next;

    __asm__ volatile("cli");
    if (current_task_index < 0) {
        panic("task killed with no scheduler running");
    }

    tasks[current_task_index].ready = false;
    next = next_ready_task(current_task_index);
    if (next < 0) {
        /* Nothing else to run.  Stopping here is better than switching to a
         * task that does not exist: the program is gone, the machine is idle,
         * and a panic says so on the serial line instead of hanging. */
        serial_write("  task: last task exited, halting\n");
        for (;;) {
            __asm__ volatile("cli; hlt");
        }
    }

    current_task_index = next;
    task_context_restore(&tasks[next].registers, &tasks[next].frame);
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
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
