ENTRY(start)
OUTPUT_FORMAT(elf64-x86-64)

KERNEL_VMA = 1M;

SECTIONS
{
    . = KERNEL_VMA;
    _kernel_start = .;

    .multiboot_header :
    {
        *(.multiboot_header)
    }

    .text : ALIGN(4K)
    {
        *(.text)
    }

    .rodata : ALIGN(4K)
    {
        *(.rodata)
    }

    .data : ALIGN(4K)
    {
        *(.data)
    }

    .bss : ALIGN(4K)
    {
        *(.bss)
        *(COMMON)
    }

    _kernel_end = .;
}