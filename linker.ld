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
        /* Everything the kernel image needs is above this, and the frame heap
         * deliberately starts here so it can grow without fighting the
         * linker for BSS budget. */
        kernel_end = .;
    } :data

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame*)
        *(.note*)
    }
}

/* BSS is small now: the graphics arena and the task stacks come from the
 * frame heap, so this budget is mostly the filesystem and the app state. */
ASSERT(bss_end - bss_start < 0x400000, "kernel BSS exceeds 4 MiB")
