.section .text
.code64

.global task_context_restore
.type task_context_restore, @function
task_context_restore:
    movq 40(%rsi), %rax
    pushq %rax
    movq 32(%rsi), %rax
    pushq %rax
    movq 24(%rsi), %rax
    pushq %rax
    movq 16(%rsi), %rax
    pushq %rax
    movq 8(%rsi), %rax
    pushq %rax

    movq 0(%rdi), %rax
    movq 8(%rdi), %rbx
    movq 16(%rdi), %rcx
    movq 24(%rdi), %rdx
    movq 112(%rdi), %r15
    movq 104(%rdi), %r14
    movq 96(%rdi), %r13
    movq 88(%rdi), %r12
    movq 80(%rdi), %r11
    movq 72(%rdi), %r10
    movq 64(%rdi), %r9
    movq 56(%rdi), %r8
    movq 48(%rdi), %rbp
    movq 32(%rdi), %rsi
    movq 40(%rdi), %rdi
    iretq

.global task_trampoline_entry
.type task_trampoline_entry, @function
task_trampoline_entry:
    popq %rdi
    call task_trampoline
    cli
    hlt
    jmp .

.section .note.GNU-stack,"",@progbits
