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

/* A stack for it.
 *
 * This used to be a fixed block in the kernel image's .bss, which cannot work:
 * those pages are supervisor-only, so ring 3 is refused the push that starts the
 * program.  The stack is now allocated from the user range like the program is,
 * and these labels are kept only so the image has a documented size. */
.section .bss
.align 16
.globl user_test_stack
user_test_stack:
    .skip 8192
.globl user_test_stack_top
user_test_stack_top:
    .zero 0

.section .text
/* The whole program, code and strings, as one blob.
 *
 * The strings are addressed relative to the instruction pointer, so the copy
 * has to land somewhere with its own layout intact -- the code and the two
 * messages move as a unit, at the same offsets to each other, and the entry
 * point is the copy's base. */
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

/* The end of the blob, so the loader can copy the whole thing without having to
 * work out the size from the symbols at the other end. */
.globl user_test_image_end
user_test_image_end:
    .zero 0
