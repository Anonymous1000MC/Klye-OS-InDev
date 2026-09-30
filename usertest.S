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
     * nothing else.  It takes no arguments, so unlike close(-1) it does not
     * have to be handed registers that are themselves canaries. */
    movq $999, %rax
    syscall

    /* rax is the return value, which is allowed to differ, so it is not
    /* Each check only ever touches %r11, which the ABI lets the kernel
     * destroy, and one byte of a report table through a memory operand.
     * A syscall per failure would have been the obvious way to report and
     * would have destroyed the canaries it was reporting on: write(1,..)
     * needs rax, rdi, rsi and rdx, three of which are themselves checked
     * later, so the first failure would cascade and the count would stop
     * meaning anything.  The table is in the blob, which is copied to
     * user memory and mapped writable, so it can be written without a
     * register.  A byte starts as '.' and becomes a letter on failure. */
    movq $0x1111, %r11
    cmpq %r11, %rbx
    je 2f
    movb $65, canary_report+0(%rip)
2:
    movq $0x2222, %r11
    cmpq %r11, %rbp
    je 2f
    movb $66, canary_report+1(%rip)
2:
    movq $0x3333, %r11
    cmpq %r11, %r12
    je 2f
    movb $67, canary_report+2(%rip)
2:
    movq $0x4444, %r11
    cmpq %r11, %r13
    je 2f
    movb $68, canary_report+3(%rip)
2:
    movq $0x5A5A, %r11
    cmpq %r11, %r14
    je 2f
    movb $69, canary_report+4(%rip)
2:
    movq $0x4446, %r11
    cmpq %r11, %r15
    je 2f
    movb $70, canary_report+5(%rip)
2:
    movq $0x5555, %r11
    cmpq %r11, %rdi
    je 2f
    movb $71, canary_report+6(%rip)
2:
    movq $0x6666, %r11
    cmpq %r11, %rsi
    je 2f
    movb $72, canary_report+7(%rip)
2:
    movq $0x7777, %r11
    cmpq %r11, %rdx
    je 2f
    movb $73, canary_report+8(%rip)
2:
    movq $0x8888, %r11
    cmpq %r11, %r8
    je 2f
    movb $74, canary_report+9(%rip)
2:
    movq $0x9999, %r11
    cmpq %r11, %r9
    je 2f
    movb $75, canary_report+10(%rip)
2:
    /* r10 is the sixth syscall argument and the SysV seventh, so a stub that
     * marshals arguments touches it.  It was also the scratch register the
     * sysretq selector build used, which is a clobber of its own. */
    movq $0xAAAA, %r11
    cmpq %r11, %r10
    je 2f
    movb $76, canary_report+11(%rip)
2:
    movq $1, %rax
    movq $1, %rdi
    leaq canary_report(%rip), %rsi
    movq $13, %rdx
    syscall
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

canary_report:
    .byte 0x2E, 0x2E, 0x2E, 0x2E, 0x2E, 0x2E, 0x2E, 0x2E, 0x2E, 0x2E, 0x2E, 0x2E, 0x2E
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
