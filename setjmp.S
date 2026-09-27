/* setjmp.S - the two pieces of C runtime machinery Lua's error handling needs.
 *
 * lua_pcall works by throwing with longjmp, so a freestanding Lua still needs
 * a working setjmp/longjmp pair.  The SysV x86-64 jmp_buf is eight 8 byte
 * slots, and only the callee-saved registers plus the stack and return
 * address have to be preserved.
 */

.text

.global klye_setjmp
.type klye_setjmp, @function
klye_setjmp:
    /* rdi = jmp_buf */
    movq %rbx,  0(%rdi)
    movq %rbp,  8(%rdi)
    movq %r12, 16(%rdi)
    movq %r13, 24(%rdi)
    movq %r14, 32(%rdi)
    movq %r15, 40(%rdi)
    /* the caller's stack pointer, before we push anything */
    leaq 8(%rsp), %rax
    movq %rax, 48(%rdi)
    /* where to resume: the instruction after the call to setjmp */
    movq (%rsp), %rax
    movq %rax, 56(%rdi)
    xorl %eax, %eax
    ret
.size klye_setjmp, .-klye_setjmp

.global klye_longjmp
.type klye_longjmp, @function
klye_longjmp:
    /* rdi = jmp_buf, esi = value; the ABI wants eax == 1 when value is 0 */
    movl %esi, %eax
    testl %eax, %eax
    jnz 1f
    movl $1, %eax
1:
    movq  0(%rdi), %rbx
    movq  8(%rdi), %rbp
    movq 16(%rdi), %r12
    movq 24(%rdi), %r13
    movq 32(%rdi), %r14
    movq 40(%rdi), %r15
    movq 48(%rdi), %rsp
    jmp *56(%rdi)
.size klye_longjmp, .-klye_longjmp

.section .note.GNU-stack,"",@progbits
