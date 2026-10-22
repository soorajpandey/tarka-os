/*
 * Kernel layout. Each segment gets exactly one permission set so the
 * bootloader maps the kernel W^X from the first instruction:
 *   text   R-X
 *   rodata R--
 *   data   RW-  (also holds .bss and the Limine request structures)
 * Segments are page-aligned so no page ever mixes permissions.
 */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS((1 << 0) | (1 << 2));  /* X | R */
    rodata PT_LOAD FLAGS((1 << 2));             /* R */
    data   PT_LOAD FLAGS((1 << 1) | (1 << 2));  /* W | R */
}

SECTIONS
{
    . = 0xffffffff80000000;

    __text_start = .;
    .text : {
        *(.text .text.*)
    } :text
    . = ALIGN(4096);
    __text_end = .;

    __rodata_start = .;
    .rodata : {
        *(.rodata .rodata.*)
    } :rodata
    . = ALIGN(4096);
    __rodata_end = .;

    __data_start = .;
    .data : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
        *(.data .data.*)
    } :data
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data
    . = ALIGN(4096);
    __data_end = .;

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
        *(.comment)
    }
}
