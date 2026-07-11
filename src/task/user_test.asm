; ring-3 test payload, embedded between user_test_start/end and copied into a
; fresh USER page by task::user::spawn before jumping in.
;
; must be position-independent (it's relocated): RIP-relative addressing only
; ([rel msg]), and only labels inside the blob. lives in .rodata as data - the
; CPU never executes it in place, so remap_kernel's NX on .rodata is fine.

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

    ; SYS_EXIT shouldn't return; spin rather than HLT (which would #GP from
    ; ring 3).
.hang:
    jmp .hang

msg:
    db "hello from ring 3!", 10
msg_end:

user_test_end:
