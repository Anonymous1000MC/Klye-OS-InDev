section .multiboot
align 8
multiboot_header:
    dd 0xE85250D6
    dd 0
    dd multiboot_header_end - multiboot_header
    dd 0x100000000 - (0xE85250D6 + 0 + (multiboot_header_end - multiboot_header))
    dw 5
    dw 1
    dd 20
    dd 1280
    dd 720
    dd 32
    dd 0
    dd 6
    dd 20
    dd 24
    dd 1
    dd 0
    dd 0
align 8
multiboot_header_end:
    dd 0

section .text.boot
bits 32
global _start
global multiboot_address
extern kernel_main

_start:
    cli
    cld
    mov edi, boot_data_start
    mov ecx, boot_data_end - boot_data_start
    xor eax, eax
    rep stosb

    mov edi, page_tables_start
    mov ecx, page_tables_end - page_tables_start
    xor eax, eax
    rep stosb

    mov [multiboot_address], ebx

    in al, 0x92
    test al, 2
    jnz .a20_enabled
    or al, 2
    out 0x92, al
.a20_enabled:

    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000000
    jb .no_long_mode

    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz .no_long_mode

    mov ebx, [multiboot_address]
    mov eax, [ebx]
    add ebx, 8
    mov edi, ebx

.find_framebuffer:
    mov eax, [multiboot_address]
    add eax, [eax]
    cmp edi, eax
    jae .tags_scanned
    mov eax, [edi]
    test eax, eax
    jz .tags_scanned
    cmp eax, 8
    je .framebuffer_tag
    mov ecx, [edi + 4]
    test ecx, ecx
    jz .tags_scanned
    add edi, ecx
    add edi, 7
    and edi, -8
    jmp .find_framebuffer

.framebuffer_tag:
    mov eax, [edi + 8]
    mov [framebuffer_address], eax
    mov eax, [edi + 16]
    mov [framebuffer_pitch], eax
    mov eax, [edi + 20]
    mov [framebuffer_width], eax
    mov eax, [edi + 24]
    mov [framebuffer_height], eax
    mov al, [edi + 28]
    mov [framebuffer_bpp], al
    mov al, [edi + 29]
    mov [framebuffer_type], al
    mov byte [framebuffer_valid], 1
    mov ecx, [edi + 4]
    add edi, ecx
    add edi, 7
    and edi, -8
    jmp .find_framebuffer

.tags_scanned:
    jmp .paging_setup

.no_framebuffer:
    mov byte [framebuffer_type], 0xFF

.no_long_mode:
    cli
    hlt
    jmp .no_long_mode

.paging_setup:
    mov eax, page_table_pdpt_framebuffer
    or eax, 3
    mov [page_table_pml4], eax

    mov dword [page_table_pdpt_framebuffer], 0x83
    mov dword [page_table_pdpt_framebuffer + 8], 0x40000083
    mov dword [page_table_pdpt_framebuffer + 16], 0x80000083
    mov dword [page_table_pdpt_framebuffer + 24], 0xC0000083
    mov dword [page_table_pdpt_framebuffer + 32], 0x83
    mov dword [page_table_pdpt_framebuffer + 36], 1

    cmp byte [framebuffer_valid], 0
    je .enable_paging

.enable_paging:
    lgdt [gdt64_pointer]

    mov eax, cr4
    or eax, (1 << 5) | (1 << 7)
    mov cr4, eax

    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    mov eax, page_table_pml4
    mov cr3, eax

    mov eax, cr0
    or eax, 0x80000001
    mov cr0, eax

    jmp 0x08:long_mode

bits 64
long_mode:
    mov eax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov rsp, stack_top
    xor rbp, rbp

    mov rdi, [rel framebuffer_address]
    mov esi, [rel framebuffer_pitch]
    mov edx, [rel framebuffer_width]
    mov ecx, [rel framebuffer_height]
    movzx r8d, byte [rel framebuffer_bpp]
    movzx r9d, byte [rel framebuffer_type]
    cld
    call kernel_main

.halt:
    cli
    hlt
    jmp .halt

bits 64
section .rodata
align 16
align 16
gdt32:
    dq 0
gdt32_code:
    dq 0x00AF9A000000FFFF
gdt32_data:
    dq 0x00CF92000000FFFF
gdt32_pointer:
    dw gdt32_pointer - gdt32 - 1
    dd gdt32

align 16
gdt64:
    dq 0
gdt64_code:
    dq 0x00AF9A000000FFFF
gdt64_data:
    dq 0x00CF92000000FFFF

align 16
gdt64_pointer:
    dw gdt64_pointer - gdt64 - 1
    dq gdt64

section .bss nobits
align 16
boot_data_start:
multiboot_address: resd 1
framebuffer_address: resq 1
framebuffer_pitch: resd 1
framebuffer_width: resd 1
framebuffer_height: resd 1
framebuffer_bpp: resb 1
framebuffer_type: resb 1
framebuffer_valid: resb 1
framebuffer_page: resd 1
boot_data_end:

section .bss.page_tables nobits align=4096
page_tables_start:
page_table_pml4: resb 4096
page_table_pdpt_identity: resb 4096
page_table_pd_identity: resb 4096
page_table_pdpt_framebuffer: resb 4096
page_table_pd_framebuffer: resb 4096
page_tables_end:

section .bss.stack nobits align=16
stack_bottom:
    resb 65536
stack_top:
