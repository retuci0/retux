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
        *(.text .text.*)
        _text_end = .;
    }

    .rodata : ALIGN(4K)
    {
        _rodata_start = .;
        *(.rodata .rodata.*)
        *(.eh_frame .eh_frame.*)
        _rodata_end = .;
    }

    .data : ALIGN(4K)
    {
        _data_start = .;
        *(.data .data.*)
        _data_end = .;
    }

    .bss : ALIGN(4K)
    {
        _bss_start = .;
        *(.bss .bss.*)
        *(.lbss .lbss.*)
        *(COMMON)
        _bss_end = .;
    }

    _kernel_end = .;
}
