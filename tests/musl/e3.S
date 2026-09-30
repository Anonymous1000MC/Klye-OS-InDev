/* mmap a page, write to it, read it back.  musl's stdio buffer comes from
 * here, so if this fails every buffered write fails after it. */
    .section .text
    .globl _start
_start:
    /* mmap(NULL, 8192, PROT_READ|WRITE, MAP_PRIVATE|ANON, -1, 0) */
    xorq %rdi, %rdi
    movq $8192, %rsi
    movq $3, %rdx
    movq $0x22, %r10
    movq $-1, %r8
    xorq %r9, %r9
    movq $9, %rax
    syscall
    movq %rax, %r12
    /* write "MMAP-OK\n" there */
    leaq s_msg(%rip), %rsi
    movq %r12, %rdi
    movq $8, %rdx
    movq $1, %rax
    syscall
    /* read it back */
    movq $0, %rax
    movq %r12, %rdi
    movq %r12, %rsi
    movq $8, %rdx
    syscall
    movq $1, %rax
    movq $1, %rdi
    movq %r12, %rsi
    movq $8, %rdx
    syscall
    /* also print the address so we can see where it landed */
    movq $1, %rax
    movq $1, %rdi
    leaq s_at(%rip), %rsi
    movq $24, %rdx
    syscall
    movq $60, %rax
    movq $42, %rdi
    syscall
1:  jmp 1b
    .section .rodata
s_msg: .ascii "MMAP-OK\n"
s_at:  .ascii "mmap returned 0x0000\n"
