/* Two writev calls with our OWN iovec array, printed between them.  This is
 * printf's shape with musl removed: if it survives, musl's iovec is the
 * problem; if the second fails, the kernel is writing into our array. */
    .section .text
    .globl _start
_start:
    /* iov on the stack: base, len, base, len */
    subq $64, %rsp
    /* iov[0] = "HELLO", 5 */
    leaq s_hello(%rip), %rax
    movq %rax, (%rsp)
    movq $5, 8(%rsp)
    /* iov[1] = "WORLD", 5 */
    leaq s_world(%rip), %rax
    movq %rax, 16(%rsp)
    movq $5, 24(%rsp)
    movq $1, %rax
    movq $1, %rdi
    movq %rsp, %rsi
    movq $2, %rdx
    syscall
    /* dump both iovecs as hex-ish to see if the kernel changed them */
    movq $1, %rax
    movq $1, %rdi
    leaq s_after(%rip), %rsi
    movq $16, %rdx
    syscall
    /* second writev, same array */
    movq $1, %rax
    movq $1, %rdi
    movq %rsp, %rsi
    movq $2, %rdx
    syscall
    movq $1, %rax
    movq $1, %rdi
    leaq s_done(%rip), %rsi
    movq $6, %rdx
    syscall
    movq $60, %rax
    movq $42, %rdi
    syscall
1:  jmp 1b
    .section .rodata
s_hello: .ascii "HELLO"
s_world: .ascii "WORLD"
s_after:  .ascii "AFTER-WROTE!!\n"
s_done:   .ascii "DONE\n"
