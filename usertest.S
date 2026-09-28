.section .text
.code64

/* A program in ring 3, to prove the privilege switch and the syscall path
 * before anything is loaded into it.
 *
 * It does the smallest thing that can be wrong: make a syscall, come back,
 * make another, and then stop.  The coming back is the part worth testing.  A
 * syscall entry that works but a return that does not leaves the processor in
 * ring 3 with the kernel's idea of the machine unchanged, and the next thing
 * that happens is a fault with no stack to describe it. */

/* A stack for it, in the kernel image's own memory.  Low and identity mapped,
 * which is what a ring 3 stack has to be until the mapping window is used for
 * user memory too. */
.section .bss
.align 16
.globl user_test_stack
user_test_stack:
    .skip 8192
/* a .skip moves past the bytes without naming what is now the end */
.globl user_test_stack_top
user_test_stack_top:
    .zero 0

.section .text
.globl user_test_entry
.type user_test_entry, @function
user_test_entry:
    /* write(1, message, length) */
    movq $1, %rax
    movq $1, %rdi
    leaq user_test_message(%rip), %rsi
    movq $user_test_message_length, %rdx
    syscall

    /* the return address has to be the one after the instruction, so a
     * syscall that comes back to the wrong place shows up here as a fault
     * rather than as a wrong answer */
    movq $7, %rbx

    /* write again, to show the first one did not just get lucky */
    movq $1, %rax
    movq $1, %rdi
    leaq user_test_second(%rip), %rsi
    movq $user_test_second_length, %rdx
    syscall

    /* exit(7) */
    movq $60, %rax
    movq $7, %rdi
    syscall

    /* the kernel has what it needs; stop rather than fall off the end */
1:
    jmp 1b

user_test_message:
    .ascii "  user: ring 3 is running\n"
    .set user_test_message_length, . - user_test_message
user_test_second:
    .ascii "  user: and it came back\n"
    .set user_test_second_length, . - user_test_second
