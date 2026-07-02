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
        _text_start = .;
        *(.text)
        _text_end = .;
    }

    .rodata : ALIGN(4K)
    {
        _rodata_start = .;
        *(.rodata)
        _rodata_end = .;
    }

    .data : ALIGN(4K)
    {
        _data_start = .;
        *(.data)
        _data_end = .;
    }

    .bss : ALIGN(4K)
    {
        _bss_start = .;
        *(.bss)
        *(COMMON)
        _bss_end = .;
    }

    _kernel_end = .;
}