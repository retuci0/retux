; one stub per CPU exception vector (0-31). each stub's only job is to make
; the stack look identical regardless of whether the CPU pushed an error
; code or not, then hand off to a single common handler.

extern isr_common_handler

bits 64
section .text

; vectors the CPU does NOT push an error code for: push a dummy 0 so the
; stack shape matches the ones that do
%macro ISR_NOERR 1
global isr%1
isr%1:
    push 0
    push %1
    jmp isr_common_stub
%endmacro

; vectors the CPU DOES push an error code for - it's already on the stack,
; we just add the vector number on top of it.
%macro ISR_ERR 1
global isr%1
isr%1:
    push %1
    jmp isr_common_stub
%endmacro

ISR_NOERR 0   ; #DE  divide error
ISR_NOERR 1   ; #DB  debug
ISR_NOERR 2   ;      non-maskable interrupt
ISR_NOERR 3   ; #BP  breakpoint
ISR_NOERR 4   ; #OF  overflow
ISR_NOERR 5   ; #BR  bound range exceeded
ISR_NOERR 6   ; #UD  invalid opcode
ISR_NOERR 7   ; #NM  device not available
ISR_ERR   8   ; #DF  double fault
ISR_NOERR 9   ;      (legacy) coprocessor segment overrun
ISR_ERR   10  ; #TS  invalid TSS
ISR_ERR   11  ; #NP  segment not present
ISR_ERR   12  ; #SS  stack-segment fault
ISR_ERR   13  ; #GP  general protection fault
ISR_ERR   14  ; #PF  page fault
ISR_NOERR 15  ;      reserved
ISR_NOERR 16  ; #MF  x87 floating-point exception
ISR_ERR   17  ; #AC  alignment check
ISR_NOERR 18  ; #MC  machine check
ISR_NOERR 19  ; #XM  SIMD floating-point exception
ISR_NOERR 20  ; #VE  virtualization exception
ISR_NOERR 21  ; #CP  pontrol protection exception
ISR_NOERR 22  ;      reserved
ISR_NOERR 23  ;      reserved
ISR_NOERR 24  ;      reserved
ISR_NOERR 25  ;      reserved
ISR_NOERR 26  ;      reserved
ISR_NOERR 27  ;      reserved
ISR_NOERR 28  ;      reserved
ISR_NOERR 29  ;      reserved
ISR_NOERR 30  ;      reserved
ISR_NOERR 31  ;      reserved


; common path for every exception
isr_common_stub:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; first argument per the System V ABI goes in RDI.
    mov rdi, rsp
    call isr_common_handler

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16  ; discard our vector number + error code
    iretq

; a table of stub addresses, so idt.cpp can fill 32 IDT entries with a loop
section .rodata
global isr_stub_table
isr_stub_table:
    dq isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7
    dq isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15
    dq isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
    dq isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31