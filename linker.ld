/*
 * linker script
 *
 * tells the linker where to place the code in memory
 * kernel starts at 1MB (0x100000) because the first 1MB is reserved by the BIOS for legacy hardware
 *
 * grub reads the layout and loads the kernel to the right address
 */

/* execution begins at _start defined in boot.asm */
ENTRY(_start)

SECTIONS
{
    /* start placing sections at 1MB */
    . = 0x100000;

    /* .multiboot comes first so grub finds the header within the first 8KB of the file */
    .multiboot ALIGN(4) :
    {
        *(.multiboot)
    }

    /* .text = executable code */
    .text ALIGN(4096) :
    {
        *(.text)
    }

    /* .rodata = read-only data */
    .rodata ALIGN(4096) :
    {
        *(.rodata)
    }

    /* .data = initialized global variables */
    .data ALIGN(4096) :
    {
        *(.data)
    }

    /* .bss = uninitialized global variables (including the stack) */
    .bss ALIGN(4096) :
    {
        *(.bss)
    }
}
