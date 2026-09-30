/* rdi across syscall 999, written in assembly so there is no register
 * allocator between the intent and the instructions. */
    .section .text
    .globl _start
_start:
    movq $999, %rax
    movq $5, %rdi
    syscall
    /* rdi should still be 5 */
    cmpq $5, %rdi
    je 2f
    /* wrong: print "RDI=WRONG\n" */
    movq $1, %rax
    movq $1, %rdi
    leaq msg_wrong(%rip), %rsi
    movq $11, %rdx
    syscall
    movq $60, %rax
    xorq %rdi, %rdi
    syscall
2:
    movq $1, %rax
    movq $1, %rdi
    leaq msg_ok(%rip), %rsi
    movq $10, %rdx
    syscall
    movq $60, %rax
    movq $42, %rdi
    syscall
1:  jmp 1b
    .section .rodata
msg_ok:
    .ascii "RDI=5 OK\n"
msg_wrong:
    .ascii "RDI=WRONG\n"
