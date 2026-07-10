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
; second PDPT for the "physmap" - the SAME 4 PDs above, reachable through a
; second PML4 slot (see set_up_page_tables below). lets the kernel turn any
; physical address into a valid pointer (`vmm::phys_to_virt`) regardless of
; which CR3 is currently loaded - load-bearing once per-task page tables
; exist and stop sharing the low identity map wholesale (mem/vmm.cpp).
p3_table_physmap: resb 4096
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

    ; physmap: a SECOND PDPT pointing at the exact same 4 PDs above, reached
    ; through PML4 index 384 (virtual base 0xFFFF'C000'0000'0000 - see
    ; mem/vmm.cpp's PHYSMAP_BASE) instead of PML4 index 0. no new physical
    ; frames - just a second path to the identical PD/2MB-huge-page entries,
    ; so the same physical range is reachable both via the low identity map
    ; AND via this fixed high-half window.
    mov eax, p2_table
    or eax, 0b11
    mov [p3_table_physmap], eax

    mov eax, p2_table2
    or eax, 0b11
    mov [p3_table_physmap + 8], eax

    mov eax, p2_table3
    or eax, 0b11
    mov [p3_table_physmap + 16], eax

    mov eax, p2_table4
    or eax, 0b11
    mov [p3_table_physmap + 24], eax

    ; install the physmap PDPT at PML4[384] (byte offset 384*8 = 0xC00).
    mov eax, p3_table_physmap
    or eax, 0b11
    mov [p4_table + 0xC00], eax

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

    ; enable SSE. the kernel's own code never emits SSE instructions (built
    ; with -mgeneral-regs-only) so this is purely for ring-3: SSE2 is part
    ; of the mandatory x86-64 SysV ABI baseline, and any real compiler
    ; output - not just hand-picked "SSE-free" code - assumes it's on. musl's
    ; own startup path (`__set_thread_area`) uses `movq %rbx,%xmm0`
    ; unconditionally, so without this every real ELF binary #UDs before
    ; main() is ever reached.
    ;
    ; CR0: clear EM (bit 2, "no x87/SSE, #UD instead") and set MP (bit 1,
    ; "WAIT/FWAIT obey TS") - the standard pairing for enabling FP/SSE.
    mov eax, cr0
    and eax, ~(1 << 2)
    or  eax, 1 << 1
    mov cr0, eax

    ; CR4: OSFXSR (bit 9, FXSAVE/FXRSTOR + actually allows SSE) and
    ; OSXMMEXCPT (bit 10, unmasked SIMD FP exceptions land as #XM/19
    ; instead of silently corrupting state).
    mov eax, cr4
    or eax, (1 << 9) | (1 << 10)
    mov cr4, eax

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
; layout constrained by both SYSRET and SYSCALL:
;   SYSCALL loads CS = STAR[47:32],    SS = STAR[47:32] + 8
;   SYSRET  loads CS = STAR[63:48]+16, SS = STAR[63:48] + 8   (64-bit form)
; so with STAR[47:32] = 0x08 and STAR[63:48] = 0x10:
;   kernel_code at 0x08, kernel_data at 0x10,
;   user_data   at 0x18, user_code   at 0x20  (both DPL=3),
;   TSS at 0x28 (double-width descriptor).
gdt64:
    dq 0                                                ; 0x00: null descriptor
.kernel_code: equ $ - gdt64                             ; 0x08
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53)            ; executable, code/data, present, long-mode
.kernel_data: equ $ - gdt64                             ; 0x10
    dq (1<<41) | (1<<44) | (1<<47)                      ; writable, code/data, present
.user_data:   equ $ - gdt64                             ; 0x18
    dq (1<<41) | (1<<44) | (1<<47) | (3<<45)            ; writable, code/data, present, DPL=3
.user_code:   equ $ - gdt64                             ; 0x20
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53) | (3<<45)  ; exec, present, long-mode, DPL=3
.tss_segment: equ $ - gdt64                             ; 0x28

; kept for the boot stub's `mov ax, gdt64.data_segment` line below.
.data_segment: equ .kernel_data
.code_segment: equ .kernel_code

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
