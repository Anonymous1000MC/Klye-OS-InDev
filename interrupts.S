.section .text
.code64

.macro ISR_NO_ERROR number
.global isr\number
.type isr\number, @function
isr\number:
    movq $0, last_error_code(%rip)
    pushq $0
    pushq $\number
    jmp isr_common
.endm

/* Written by the error-code entries before the code is dropped, and read by
 * capture_fault.  The frame itself carries a zero in this slot, because the
 * two entry shapes have to agree on where the vector is; that normalisation is
 * worth having, but it costs the one field that says whether a page fault was
 * a missing page, a write, or an access from ring 3, and those three need
 * different fixes. */
.global last_error_code
last_error_code:
    .quad 0

.macro ISR_ERROR number
.global isr\number
.type isr\number, @function
isr\number:
    movq (%rsp), %rax
    movq %rax, last_error_code(%rip)
    addq $8, %rsp
    pushq $0
    pushq $\number
    jmp isr_common
.endm

ISR_NO_ERROR 0
ISR_NO_ERROR 1
ISR_NO_ERROR 2
ISR_NO_ERROR 3
ISR_NO_ERROR 4
ISR_NO_ERROR 5
ISR_NO_ERROR 6
ISR_NO_ERROR 7
ISR_ERROR 8
ISR_NO_ERROR 9
ISR_ERROR 10
ISR_ERROR 11
ISR_ERROR 12
ISR_ERROR 13
ISR_ERROR 14
ISR_NO_ERROR 15
ISR_NO_ERROR 16
ISR_ERROR 17
ISR_NO_ERROR 18
ISR_NO_ERROR 19
ISR_NO_ERROR 20
ISR_ERROR 21
ISR_NO_ERROR 22
ISR_NO_ERROR 23
ISR_NO_ERROR 24
ISR_NO_ERROR 25
ISR_NO_ERROR 26
ISR_NO_ERROR 27
ISR_ERROR 28
ISR_ERROR 29
ISR_ERROR 30
ISR_NO_ERROR 31

ISR_NO_ERROR 32
ISR_NO_ERROR 33
ISR_NO_ERROR 34
ISR_NO_ERROR 35
ISR_NO_ERROR 36
ISR_NO_ERROR 37
ISR_NO_ERROR 38
ISR_NO_ERROR 39
ISR_NO_ERROR 40
ISR_NO_ERROR 41
ISR_NO_ERROR 42
ISR_NO_ERROR 43
ISR_NO_ERROR 44
ISR_NO_ERROR 45
ISR_NO_ERROR 46
ISR_NO_ERROR 47
ISR_NO_ERROR 255

/* The syscall entry, vector 0x80.
 *
 * Separate from isr_common on purpose.  A gate has to offer ring 3 the
 * privilege it is reached at, and the exception gates deliberately do not: they
 * are DPL 0, so ring 3 is refused with a general protection fault.  This one
 * carries DPL 3 in the low two bits of its type byte.
 *
 * It also does not go through the interrupt path.  A syscall is a call, and
 * the frame it has to leave behind is the one ring3.S's own stub builds, with
 * the register save and the return to user mode done in assembly where the
 * exact stack layout is known. */
.global isr_syscall
.type isr_syscall, @function
isr_syscall:
    jmp syscall_entry

isr_common:
    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %r11
    pushq %r10
    pushq %r9
    pushq %r8
    pushq %rbp
    pushq %rdi
    pushq %rsi
    pushq %rdx
    pushq %rcx
    pushq %rbx
    pushq %rax
    movq %rsp, %rdi
    movq 120(%rsp), %rsi
    call interrupt_dispatch
    popq %rax
    popq %rbx
    popq %rcx
    popq %rdx
    popq %rsi
    popq %rdi
    popq %rbp
    popq %r8
    popq %r9
    popq %r10
    popq %r11
    popq %r12
    popq %r13
    popq %r14
    popq %r15
    addq $16, %rsp
    iretq

.section .rodata
.global isr_table
isr_table:
    .quad isr0
    .quad isr1
    .quad isr2
    .quad isr3
    .quad isr4
    .quad isr5
    .quad isr6
    .quad isr7
    .quad isr8
    .quad isr9
    .quad isr10
    .quad isr11
    .quad isr12
    .quad isr13
    .quad isr14
    .quad isr15
    .quad isr16
    .quad isr17
    .quad isr18
    .quad isr19
    .quad isr20
    .quad isr21
    .quad isr22
    .quad isr23
    .quad isr24
    .quad isr25
    .quad isr26
    .quad isr27
    .quad isr28
    .quad isr29
    .quad isr30
    .quad isr31

.global irq_table
irq_table:
    .quad isr32
    .quad isr33
    .quad isr34
    .quad isr35
    .quad isr36
    .quad isr37
    .quad isr38
    .quad isr39
    .quad isr40
    .quad isr41
    .quad isr42
    .quad isr43
    .quad isr44
    .quad isr45
    .quad isr46
    .quad isr47

.section .note.GNU-stack,"",@progbits
