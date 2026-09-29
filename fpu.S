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

    /* The AVX register state, so VEX-encoded instructions can be decoded.
     *
     * A VEX prefix -- the 0xC5 that starts `vpxor %xmm0,%xmm0,%xmm0` -- is not
     * just a different encoding of the same SSE operation.  Decoding one
     * requires both CR4.OSXSAVE, which permits xgetbv and xsetbv at all, and
     * XCR0 bit 2 saying the AVX state is enabled.  With either missing the
     * instruction raises invalid opcode, and it does so as a *decode* failure
     * before the bytes are read -- so it arrives looking like a corrupt
     * instruction stream in a program that is entirely fine.
     *
     * This is not optional for running real programs.  A glibc built for a
     * modern distribution carries VEX-encoded instructions even in a static
     * binary, including in the ifunc resolver code that runs during startup:
     * a stock -static-pie built against the system libc contains thousands of
     * them, and _dl_aux_init, which every glibc program calls during startup,
     * begins with a vpxor.  Compiling the program's own source for baseline
     * x86-64 does not help, because the instructions are in the prebuilt libc
     * objects, not in the code being compiled.
     *
     * Both parts are guarded by what the processor reports.  Issuing xsetbv
     * requires CPUID.1:ECX bit 26; where that is clear the instruction is
     * itself an invalid opcode and the machine dies before the first serial
     * byte, with no message at all.  So it is asked rather than assumed. */
    mov %cr4, %rax
    andq $(~(1 << 18)), %rax     /* clear OSXSAVE while we look */
    mov %rax, %cr4
    movl $1, %eax                /* CPUID leaf 1, subleaf 0 */
    xorl %ecx, %ecx
    pushq %rbx                    /* cpuid overwrites ebx, and this is a
                                   * callee saved register, so the caller's
                                   * value of it has to survive this call */
    cpuid
    popq %rbx
    test $(1 << 26), %ecx          /* XSAVE, which is in ECX: CPUID puts the
                                   * feature flags in ecx and edx, and eax
                                   * holds the highest leaf, so testing eax
                                   * tests the number 1 against the bit and
                                   * always fails */
    jz 1f                         /* no XSAVE: AVX cannot be enabled */
    mov %cr4, %rax
    orq  $(1 << 18), %rax         /* CR4.OSXSAVE */
    mov %rax, %cr4
    xorl %ecx, %ecx              /* xsetbv takes the index in ecx */
    movl $0x7, %eax              /* XCR0: x87 | SSE | AVX */
    /* xsetbv, and the REX.W prefix is not optional.
     *
     * xsetbv and xgetbv are the same opcode, 0F 01 D0, and differ only in
     * whether REX.W is present: 48 0F 01 D0 writes, 0F 01 D0 reads.  Emitted
     * bare, this was xgetbv -- it read XCR0, discarded the result, and enabled
     * nothing, so every VEX instruction below still raised invalid opcode and
     * nothing about the fault said why. */
    .byte 0x48, 0x0f, 0x01, 0xd0  /* xsetbv */
1:
    ret
.size fpu_enable, .-fpu_enable

.section .note.GNU-stack,"",@progbits
