; ring-3 test payload. embedded as bytes in the kernel image via a global
; label pair (`user_test_start`, `user_test_end`), then `task::user::spawn`
; copies the whole blob into a fresh `USER|PRESENT` page and drops into it.
;
; keep everything position-independent so the copy works: use ONLY
; RIP-relative addressing (`[rel msg]`), and only reference labels that
; are themselves inside the blob. RIP-relative displacements are encoded
; as `target - next_instruction`, both of which shift by the same amount
; when the blob is relocated, so the resulting effective address comes
; out right at the destination.
;
; this file is assembled with `bits 64` and lives in `.rodata` - the CPU
; never executes it in place; it's data to be copied and executed
; elsewhere. that's also why the kernel's own W^X mapping of `.rodata`
; (no-execute, per `vmm::remap_kernel`) doesn't get in the way.

bits 64
section .rodata

global user_test_start
global user_test_end

user_test_start:
    ; write(1, msg, msg_len);
    mov rax, 1                          ; SYS_WRITE
    mov rdi, 1                          ; fd = 1 (tty)
    lea rsi, [rel msg]                  ; buf - RIP-relative, position-independent
    mov rdx, msg_end - msg              ; len - assembled as a constant
    syscall

    ; exit(0);
    mov rax, 60                         ; SYS_EXIT
    xor rdi, rdi                        ; status = 0
    syscall

    ; SYS_EXIT shouldn't return, but be defensive - a HLT from ring 3
    ; would #GP and take us to the exception handler, at which point at
    ; least the register dump makes clear what went wrong. keep spinning
    ; instead - safer footgun-wise.
.hang:
    jmp .hang

msg:
    db "hello from ring 3!", 10
msg_end:

user_test_end:
