OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x100000;

    .boot ALIGN(8) :
    {
        KEEP(*(.multiboot))
    } :text

    .text ALIGN(4K) :
    {
        KEEP(*(.text.boot))
        *(.text .text.*)
    } :text

    .rodata ALIGN(4K) :
    {
        *(.rodata .rodata.*)
    } :text

    .data ALIGN(4K) :
    {
        *(.data .data.*)
    } :data

    .bss ALIGN(4K) :
    {
        bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        bss_end = .;
    } :data

    .gfxbuf ALIGN(4K) :
    {
        gfx_buffer_start = .;
        *(.gfxbuf .gfxbuf.*)
        gfx_buffer_end = .;
    } :data

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame*)
        *(.note*)
    }
}

ASSERT(bss_end - bss_start < 0xE00000, "kernel BSS exceeds 14 MiB")
ASSERT(gfx_buffer_end - gfx_buffer_start <= 0x900000, "G back buffer too large")
ASSERT(gfx_buffer_end < 0x1000000, "G back buffer must stay below 16 MiB")
