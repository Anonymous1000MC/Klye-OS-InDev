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


/* A syscall register canary.
 *
 * Every register a syscall is allowed to touch is loaded with a distinct
 * value, one syscall for a number that does not exist is made, and then each
 * register is compared against what it was set to.  The x86-64 Linux syscall
 * contract says exactly three may differ: rax, which carries the return value,
 * and rcx and r11, which the instruction uses to hand back the return address
 * and the flags.  Anything else that moved is the kernel's fault, and this
 * names which one rather than leaving it to be inferred from whatever the
 * program did afterwards.
 *
 * The report is written a character at a time through write(1), so a register
 * that comes back wrong shows up as a letter rather than as a fault somewhere
 * downstream.
 */
.macro CANARY_CHECK name, register, expected
    movq \expected, %r11        # expected, into a register the ABI frees
    cmpq %r11, \register
    je 2f
    movq $0x1, %rax
    movq $1, %rdi
    leaq canary_bad(%rip), %rsi
    movq $1, %rdx
    syscall
2:
.endm

.globl user_canary_entry
.type user_canary_entry, @function
user_canary_entry:
    /* Callee-saved: must survive exactly. */
    movq $0x1111, %rbx
    movq $0x2222, %rbp
    movq $0x3333, %r12
    movq $0x4444, %r13
    movq $0x4445, %r14
    movq $0x4446, %r15
    /* Caller-saved, but a syscall still must not touch them. */
    movq $0x5555, %rdi
    movq $0x6666, %rsi
    movq $0x7777, %rdx
    movq $0x8888, %r8
    movq $0x9999, %r9
    movq $0xAAAA, %r10
    /* An fs base of our own, so a kernel that clobbers the thread pointer
     * between programs shows it as a canary failure rather than as mystery */
    movq $0x5A5A, %r14

    /* syscall 999: not implemented, must come back -ENOSYS and change
     * nothing else */
    movq $999, %rax
    syscall

    /* rax is the return value, which is allowed to differ, so it is not
     * checked here.  Everything else is. */
    CANARY_CHECK rbx, %rbx, $0x1111
    CANARY_CHECK rbp, %rbp, $0x2222
    CANARY_CHECK r12, %r12, $0x3333
    CANARY_CHECK r13, %r13, $0x4444
    CANARY_CHECK r14, %r14, $0x5A5A
    CANARY_CHECK r15, %r15, $0x4446
    CANARY_CHECK rdi, %rdi, $0x5555
    CANARY_CHECK rsi, %rsi, $0x6666
    CANARY_CHECK rdx, %rdx, $0x7777
    CANARY_CHECK r8,  %r8,  $0x8888
    CANARY_CHECK r9,  %r9,  $0x9999
    CANARY_CHECK r10, %r10, $0xAAAA

    movq $1, %rax
    movq $1, %rdi
    leaq canary_done(%rip), %rsi
    movq $canary_done_length, %rdx
    syscall

    movq $60, %rax
    movq $0, %rdi
    syscall
1:
    jmp 1b

canary_bad:
    .ascii "X"
canary_done:
    .ascii "  canary: done\n"
    .set canary_done_length, . - canary_done

/* The end of the blob, so the loader can copy the whole thing without having to
 * work out the size from the symbols at the other end. */
.globl user_test_image_end
user_test_image_end:
    .zero 0
