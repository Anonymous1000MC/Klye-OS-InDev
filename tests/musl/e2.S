/* Reproduce printf's shape exactly: writev with TWO iovecs where the second
 * is uninitialised, which is what musl's second call does.  If this works,
 * the kernel handles a partial iovec list and musl's state is at fault. */
    .section .text
    .globl _start
_start:
    subq $64, %rsp
    /* iov[0] valid, iov[1] deliberately garbage */
    leaq s_hello(%rip), %rax
    movq %rax, (%rsp)
    movq $3, 8(%rsp)
    movq $0xdeadbeef, 16(%rsp)
    movq $0xffffffffffffffea, 24(%rsp)
    movq $1, %rax
    movq $1, %rdi
    movq %rsp, %rsi
    movq $2, %rdx          /* TWO entries, second is garbage */
    syscall
    movq $60, %rax
    movq $42, %rdi
    syscall
1:  jmp 1b
    .section .rodata
s_hello: .ascii "OK\n"
