.section .text
.code64

/* Ring 3.
 *
 * Three pieces: an entry that raises the privilege level for the first time,
 * a syscall entry, and the way back out of a syscall.
 *
 * The syscall entry is the awkward one.  Unlike an interrupt, the syscall
 * instruction does not change privilege level and does not touch the stack, so
 * the stub has to find a kernel stack and remember where the user's was.  It
 * also has to preserve the return state: the instruction leaves the return
 * address in rcx and the flags in r11, and both are overwritten by the frame
 * the stub builds and by whatever the handler calls.  Keeping them in two
 * fixed words is enough because there is one ring 3 task, which is stated
 * here rather than left to be discovered when a second one arrives.
 */

.section .bss
.align 8
.globl user_syscall_rip
user_syscall_rip:
    .quad 0
.globl user_syscall_rflags
user_syscall_rflags:
    .quad 0
/* The user's stack pointer at the syscall instruction.
 *
 * It cannot be kept in rbp.  rbp is part of the register frame the handler is
 * given, so the save and the restore are the user's own rbp, and by the time
 * the return path runs, rbp holds whatever the program had rather than the
 * stack it was on.  Restoring the stack pointer from that puts the program
 * back on a stack pointer that was never its own, and the next instruction
 * writes to an address below the bottom of the stack, which is mapped, so
 * there is no fault to explain it: the program just stops. */
.globl user_syscall_stack
user_syscall_stack:
    .quad 0

/* The thread pointer the user program set with arch_prctl(ARCH_SET_FS).
 *
 * Written here rather than into MSR_FS_BASE directly, and applied at the two
 * transitions instead.  FS base is per-thread hardware state that the CPU does
 * not save across a task switch by itself, so a value written once at startup
 * would still be correct for a single program and wrong the moment a second
 * one ran with a different stack.  Applying it on the way in and on the way out
 * of ring 3 costs two instructions and cannot drift. */
.globl user_fs_base_slot
user_fs_base_slot:
    .quad 0

/* The kernel stack a syscall runs on, at a fixed address so the stub can
 * reach it without needing any data of its own to find it. */
.align 16
.globl syscall_kernel_stack
syscall_kernel_stack:
    .skip 16384
/* the top of that stack: a .skip moves past the bytes without naming what is
 * now the end, so the label has to be put there by hand */
.globl syscall_kernel_stack_top
syscall_kernel_stack_top:
    .zero 0

/* A second stack, and a more important one.
 *
 * When an interrupt arrives from ring 3 the processor does not use the
 * interrupted stack.  It loads the stack pointer from the third field of the
 * task state segment, which is where a program left off, and that is the only
 * thing standing between a timer tick and a fault.  A task state segment with a
 * zero there does not fail when it is loaded; it fails once a millisecond
 * later, at the first tick, with registers belonging to whatever else happened
 * to be running, which is the least informative panic there is.
 *
 * It is separate from the syscall stack on purpose.  A syscall can be entered
 * from inside an interrupt, and one stack shared between the two means the two
 * overwrite each other's frames. */
.align 16
.globl user_kernel_stack
user_kernel_stack:
    .skip 16384
.globl user_kernel_stack_end
user_kernel_stack_end:
    .zero 0

.section .text

/* The markers are C functions, so the serial port code is not written twice.
 * They are here because a fault on this path can end in a triple fault, which
 * resets the processor before anything can report itself: the only way to know
 * how far execution got is to have said so on the way in. */
.extern serial_marker_ptr_enter
.extern serial_marker_ptr_in_kernel
.extern serial_marker_ptr_return

/* Entering ring 3 for the first time.
 *
 * iretq is the only instruction that raises the privilege level, and it takes
 * the whole return state off the stack in order: instruction pointer, code
 * selector, flags, stack pointer, stack selector.  The flags are not optional
 * detail: leave the interrupt flag clear and the first timer tick in ring 3
 * arrives as a fault.
 *
 * rcx and r11 are set here rather than restored by iretq, because they are
 * what the first syscall returns through.  sysretq takes the flags from r11's
 * low half and the ring 0 segment selectors from its high half, so r11 is
 * built as flags below and selectors above. */
.globl user_enter
.type user_enter, @function
user_enter:
    /* iretq pops the frame from the top down: instruction pointer, code
     * selector, flags, stack pointer, stack selector.  Pushing pushes the
     * stack pointer down, so the fields go on in the reverse of the order they
     * come off.  Pushing them the natural way round puts the code selector on
     * top, and the processor then takes the code selector as the instruction
     * pointer and the instruction pointer as the code selector, which is a
     * general protection fault naming neither field.
     *
     * The interrupt flag is deliberately left clear.  Raising privilege from
     * inside a scheduler task and then taking an interrupt is a mess this is
     * not ready for: the tick switches to the kernel through the task state
     * segment, the scheduler runs, and it comes back to a task that is half
     * way through entering ring 3.  The fault that produces names no field
     * either, and its registers belong to whichever task the scheduler
     * happened to be running.
     *
     * The test program is a handful of instructions and waits for nothing, so
     * nothing is lost by running it with interrupts off, and nothing can
     * interrupt the middle of the switch.  Doing this properly means giving
     * ring 3 a real task with its own stack and context, which is the next
     * step and not something a shell command can stand in for. */
    /* The stack selector is 0x1B, not 0x2B.
     *
     * The table is: 0 null, 1 kernel code, 2 kernel data, 3 user data, 4 user
     * code, 5 the task state segment.  User data is entry 3, and a ring 3
     * selector adds 3, so it is 0x18 + 3 = 0x1B.  0x2B would be entry 5 with
     * ring 3, which is the task state segment, and a task state segment is not
     * a stack: loading it as SS raises a general protection fault with an
     * error code of zero, which names nothing about the selector at all. */
    pushq $0x1B                   /* stack selector: user data, ring 3 */
    pushq %rsi                    /* the user's stack pointer */
    pushq $0x2                    /* flags: reserved bit set, interrupts off */
    pushq $0x23                   /* code selector: user code, ring 3 */
    pushq %rdi                    /* the user's instruction pointer */
    /* rcx and r11 are not restored by iretq, and they are what the first
     * syscall returns through, so they are set here. */
    movq %rdi, user_syscall_rip(%rip)
    movq $0x2, user_syscall_rflags(%rip)
    /* The thread pointer belongs to the program, not to the kernel: apply it
     * on the way in so the first instruction the program runs already sees its
     * own thread local storage. */
    movl $0xC0000100, %ecx      /* IA32_FS_BASE */
    movq user_fs_base_slot(%rip), %rax
    wrmsr
    call serial_marker_ptr_enter
    iretq

.globl syscall_entry
.type syscall_entry, @function
syscall_entry:
    /* Nothing here may call into the kernel.  The syscall instruction does not
     * change privilege level, so everything up to the far return below is
     * still running in ring 3 holding the user's code selector, and a direct
     * call to a ring 0 address is a privilege violation.  That is not a
     * theoretical concern: a marker function added at the top of this stub to
     * trace it faulted on the call, and a fault taken from ring 3 needs the
     * task state segment to switch stacks, which failed in turn, so the
     * machine reset with nothing on the serial port at all. */
    /* The syscall instruction does not change privilege level.  It jumps to
     * this address and leaves everything else alone: still ring 3, still
     * holding the user's code selector, still on the user's stack.  A stub
     * that treats it like an interrupt runs the whole handler in ring 3, and
     * the first privileged thing the kernel then does is the fault that
     * reports it, which is exactly how this was found: a general protection
     * fault saying a privileged operation was attempted at user level.
     *
     * So the privilege level is raised here, by hand. */
    movq %rsp, user_syscall_stack(%rip)  /* the user's stack, for the return */
    movq %rcx, user_syscall_rip(%rip)    /* the return state the */
    movq %r11, user_syscall_rflags(%rip) /* instruction left in two registers */
    movq $syscall_kernel_stack_top, %rsp

    /* A far return is the cheap way to change privilege level when there is no
     * frame to come back through: it pops an instruction pointer and a
     * selector off the stack, and loading a ring 0 selector is what drops the
     * ring.
     *
     * It pops the instruction pointer first, from the top, and pushing moves
     * the stack pointer down, so the selector goes on first for the pointer
     * to end up above it.  The other way round is the same mistake as the
     * iretq frame, and it fails worse: the processor takes the code selector as
     * the instruction pointer, faults inside a stub that is still at ring 3,
     * faults again on that, and the machine resets without a word. */
    pushq $0x08                   /* kernel code selector, lower */
    pushq $syscall_in_kernel      /* where to carry on, on top */
    lretq

syscall_in_kernel:
    /* The register save comes before anything that can use a register, and a
     * call is exactly that: it is free to clobber rax, which is where the
     * syscall number is, and free to clobber the rest.  The marker therefore
     * has to go after the save and before the dispatch, and it can no longer
     * be a call into C without saving again around it -- the trace is worth
     * less than a correct frame. */
    /* The register save has to come before anything that can use a register,
     * and a call is exactly that: it is free to clobber rax, which is where
     * the syscall number is, and free to clobber the rest.  The marker is
     * below the save for that reason, not for tidiness. */
    /* the same three words the interrupt stub builds, in the same order, so
     * one dispatcher serves both.  The return address is this instruction,
     * which nothing returns through: a syscall goes back with sysretq. */
    pushq $0                      /* error code: none for a syscall */
    pushq $0x80                   /* the vector */
    pushq $syscall_entry

    /* and the same register save the interrupt stub does */
    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %r11
    pushq %r10
    pushq %r9
    pushq %r8
    pushq %rbp
    pushq %rdi
    pushq %rsi
    pushq %rdx
    pushq %rcx
    pushq %rbx
    pushq %rax
    movq %rsp, %rdi
    movq $0x80, %rsi
    call interrupt_dispatch

    popq %rax
    popq %rbx
    popq %rcx
    popq %rdx
    popq %rsi
    popq %rdi
    popq %rbp
    popq %r8
    popq %r9
    popq %r10
    popq %r11
    popq %r12
    popq %r13
    popq %r14
    popq %r15
    addq $24, %rsp                /* the error code, the vector, the return address */
    jmp syscall_return

.globl syscall_return
.type syscall_return, @function
syscall_return:
    /* The data selectors still point at the user's descriptors, and a ring 0
     * process may not use a segment whose requested privilege is 3.  That is a
     * general protection fault on the first memory write anything here makes,
     * so they are replaced before the marker call, which is the first thing
     * that would do one. */
    movq $0x10, %rax              /* kernel data; rax is free, the frame is gone */
    movq %rax, %ds
    movq %rax, %es
    call serial_marker_ptr_return

    /* Hand the data selectors back before dropping privilege, so the program
     * does not find the kernel's descriptors loaded when it resumes.  A data
     * selector with a lower privilege than the current ring is legal, so
     * leaving them would work, and would still be impolite.  SS is not set here:
     * sysretq loads it, and it has to come from the same place as CS so the
     * two cannot disagree. */
    movq $0x1B, %rax              /* 0x18 user data, ring 3 */
    movq %rax, %ds
    movq %rax, %es
    /* Read every value out of memory before %rsp is pointed at the user's
     * stack.  Loading that stack pointer and then calling anything pushes the
     * return address onto it, and a push there is a write into the page above
     * the allocation: mapped, so no fault, and quietly wrong. */
    /* %rsp is loaded straight from memory, with no register in between.
     *
     * This used to go through %rbx, as scratch, and that is an ABI violation
     * that costs nothing to look at in a test program and everything to a real
     * one.  A syscall on x86-64 is a function call as far as the calling code
     * is concerned: it clobbers %rcx and %r11 and nothing else.  Every other
     * register, %rbx included, comes back as it went in.
     *
     * musl keeps the initial thread's control block in %rbx across the
     * arch_prctl it makes during TLS setup.  Handing it a stack pointer
     * instead means its next store -- `movl $0x2,0x38(%rbx)`, writing a flag
     * into the thread struct -- goes to an address that is not the thread
     * struct, and the program faults there with nothing to connect it to the
     * syscall that went before it.  usertest never noticed, because it does
     * not keep anything in a callee-saved register across a syscall. */
    movq user_syscall_stack(%rip), %rsp
    movq user_syscall_rip(%rip), %rcx
    movq user_syscall_rflags(%rip), %r11
    /* sysretq takes its instruction pointer from rcx and both selectors plus
     * the flags from r11: CS in bits 63:48, SS in 47:32, RFLAGS in the low
     * half.  The two selectors in STAR cannot supply that, because the field
     * sysretq derives them from has to be a ring 0 value for the syscall to
     * have been able to enter at all, so the ring 3 pair is built here.
     *
     * CS into bits 63:48 needs a 48 bit shift, not 16.  With 16 the code
     * selector lands at bit 32, bits 63:48 stay zero, and sysretq is handed a
     * null code selector -- which it rejects without raising anything, so the
     * program stops between one write and the next with no fault and no panic.
     * That is precisely the symptom this had, and it survived a hand check of
     * the arithmetic because 0x00000023001B0002 still looks reasonable read
     * from the wrong end of the register. */
    /* rcx cannot be the scratch here: it already holds the address sysretq is
     * about to jump to, and overwriting it is how the return ended up at
     * 0x1B00000000 -- the stack selector executed as code, with no fault until
     * the fetch. */
    movq $0x23, %rax
    shlq $48, %rax
    movq $0x1B, %rbx
    shlq $32, %rbx
    orq %rbx, %rax
    orq %r11, %rax
    movq %rax, %r11
    sysretq
