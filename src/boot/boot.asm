; this file's job: build minimal page tables, climb into
; 64-bit long mode, and call into kernel_main().

global start
extern kernel_main


; --- Multiboot2 header ---

; GRUB scans the first 32KB for this number
MB_MAGIC    equ 0xE85250D6
MB_ARCH     equ 0  ; 0 = i386 protected mode target

section .multiboot_header
header_start:
    dd MB_MAGIC
    dd MB_ARCH
    dd header_end - header_start
    ; checksum: the four fields above must sum to 0 (mod 2^32)
    dd 0x100000000 - (MB_MAGIC + MB_ARCH + (header_end - header_start))
    ; mandatory end tag
    dw 0      ; type
    dw 0      ; flags
    dd 8      ; size
header_end:


; --- reserved mem for our boot-time page tables and stack ---

section .bss
align 4096
p4_table:     resb 4096
p3_table:     resb 4096
p2_table:     resb 4096      ; for 0-1 GiB
p2_table2:    resb 4096      ; for 1-2 GiB
p2_table3:    resb 4096      ; for 2-3 GiB
p2_table4:    resb 4096      ; for 3-4 GiB
stack_bottom: resb 16384
stack_top:


; --- 32-bit code ---
section .text
bits 32
start:
    ; set up a custom stack pointer since GRUB does not provide one
    mov esp, stack_top

    ; stash the multiboot info pointer (EBX) into EDI which won't be touched by CPUID
    mov edi, ebx
    push edi

    call check_multiboot
    call check_cpuid
    call check_long_mode

    call set_up_page_tables
    call enable_paging

    lgdt [gdt64_pointer]
    pop edi
    jmp 0x08:long_mode_start


; --- sanity checks ---

check_multiboot:
    ; GRUB leaves this value in EAX
    cmp eax, 0x36D76289
    jne .fail
    ret
.fail:
    mov al, "M"
    jmp boot_error

check_cpuid:
    ; if the CPU actually has CPUID, the flip sticks; if not, the bit snaps back on its own.
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    cmp eax, ecx
    je .fail
    ret
.fail:
    mov al, "C"
    jmp boot_error

check_long_mode:
    ; we need at least 0x80000001 to ask about long mode at all.
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .fail

    ; extended function 0x80000001, bit 29 of EDX, is the long-mode flag.
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz .fail
    ret
.fail:
    mov al, "L"
    jmp boot_error


; --- minimal identity-mapped page table set ---
; PML4[0] -> PDPT -> PD, where the PD uses 2MB pages directly (no PT level),
; identity-mapping the first 1GB of physical memory (512 entries * 2MB).

set_up_page_tables:
    ; PML4 -> PDPT
    mov eax, p3_table
    or eax, 0b11
    mov [p4_table], eax

    ; fill PDPT entries for 4 PDs
    mov eax, p2_table
    or eax, 0b11
    mov [p3_table], eax

    mov eax, p2_table2
    or eax, 0b11
    mov [p3_table + 8], eax

    mov eax, p2_table3
    or eax, 0b11
    mov [p3_table + 16], eax

    mov eax, p2_table4
    or eax, 0b11
    mov [p3_table + 24], eax

    ; fill each PD with 512 huge pages
    mov esi, p2_table          ; first PD
    mov edi, 0                 ; base physical address (in bytes)
.fill_all:
    call .fill_pd
    add esi, 4096              ; next PD
    add edi, 0x40000000        ; next 1GiB
    cmp edi, 0x100000000       ; done at 4GiB
    jne .fill_all
    ret

.fill_pd:
    ; esi = PD address, edi = base physical address
    xor ecx, ecx
.loop:
    mov eax, edi
    mov eax, edi          ; base address
    mov edx, ecx
    shl edx, 21           ; ecx * 2,097,152
    add eax, edx
    or eax, 0b10000011
    mov [esi + ecx * 8], eax
    inc ecx
    cmp ecx, 512
    jne .loop
    ret

.map_pd_entry:
    mov eax, 0x200000        ; 2MB
    mul ecx                  ; eax = ecx * 2MB
    or eax, 0b10000011       ; present + writable + huge-page bit
    mov [p2_table + ecx * 8], eax
    inc ecx
    cmp ecx, 512
    jne .map_pd_entry
    ret

enable_paging:
    ; point CR3 at the top of our page-table hierarchy.
    mov eax, p4_table
    mov cr3, eax

    ; set CR4.PAE since long mode requires PAE-style paging.
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    ; enable OSFXSR and OSXMMEXCPT in CR4.
    ; mov eax, cr4
    ; or eax, (1 << 9) | (1 << 10)   ; OSFXSR + OSXMMEXCPT
    ; mov cr4, eax

    ; set the long mode enable bit in the EFER MSR.
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 8) | (1 << 11)  ; LME + NXE
    wrmsr

    ; set CR0.PG, this actually switches paging on.
    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax
    ret

; if anything above fails, write "ERR: <code>" directly to VGA text memory
boot_error:
    mov dword [0xb8000], 0x4f524f45  ; "ER" in white-on-red
    mov dword [0xb8004], 0x4f3a4f52  ; "R:"
    mov dword [0xb8008], 0x4f204f20  ; "  "
    mov byte  [0xb800a], al
    hlt


; --- 64-bit GDT ---
; must be in .data (not .rodata) because tss::init() writes the TSS descriptor
; into gdt64_tss_slot at runtime

section .data
align 8
gdt64:
    dq 0                                          ; 0x00: null descriptor
.code_segment: equ $ - gdt64                      ; 0x08
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53)      ; executable, code/data, present, long-mode
.data_segment: equ $ - gdt64                      ; 0x10
    dq (1<<41) | (1<<44) | (1<<47)                ; writable, code/data, present
.tss_segment:  equ $ - gdt64                      ; 0x18

; 16-byte TSS descriptor slot, zero for now, filled by tss::init() at runtime.
; a 64-bit TSS descriptor is double-width to hold the full 64-bit base address.
global gdt64_tss_slot
gdt64_tss_slot:
    dq 0   ; low  qword
    dq 0   ; high qword

; gdt64_pointer is global (not a local label) so it can be referenced
; from .text above without a forward-reference error across sections.
global gdt64_pointer
gdt64_pointer:
    dw $ - gdt64 - 1
    dq gdt64

; --- 64-bit code ---
bits 64
section .text
long_mode_start:
    mov ax, gdt64.data_segment
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; EDI held the multiboot info pointer in 32-bit mode
    call kernel_main

    ; kernel_main should never return, but if it does, halt forever.
.hang:
    hlt
    jmp .hang
