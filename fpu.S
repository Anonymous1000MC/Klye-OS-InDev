/* fpu.S - save and restore the extended register file.
 *
 * FXSAVE writes the 512 byte legacy layout: control word, status word, tag
 * word, MXCSR, the x87 stack, and XMM0-XMM15.  A task that never touches
 * floating point still gets a well defined area because the initial state is
 * zeroed by the scheduler.
 */

.text

.global task_fpu_save
.type task_fpu_save, @function
task_fpu_save:
    /* rdi = area, 16 byte aligned */
    fxsave (%rdi)
    ret
.size task_fpu_save, .-task_fpu_save

.global task_fpu_restore
.type task_fpu_restore, @function
task_fpu_restore:
    /* rdi = area */
    fxrstor (%rdi)
    ret
.size task_fpu_restore, .-task_fpu_restore

/* Enable SSE on the boot CPU.  The kernel is entered with CR4.OSFXSR clear,
 * so any SSE instruction would fault until this runs. */
.global fpu_enable
.type fpu_enable, @function
fpu_enable:
    mov %cr4, %rax
    orq $(1 << 9), %rax          /* CR4.OSFXSR */
    mov %rax, %cr4
    /* clear CR0.EM and set CR0.MP so SSE is usable */
    mov %cr0, %rax
    andq $(~(1 << 2)), %rax     /* clear EM */
    orq  $(1 << 1), %rax         /* set MP */
    mov %rax, %cr0
    ret
.size fpu_enable, .-fpu_enable

.section .note.GNU-stack,"",@progbits
