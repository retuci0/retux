; fork_enter_ring3(const ForkFrame* f) - never returns.
;
; loads the callee-saved regs sys_fork() (task/fork.cpp) captured from the
; parent's syscall, forces rax = 0 (the child's return; the parent gets the
; child pid through the normal dispatch path), and IRETQs into ring 3 at the
; parent's rip/rflags/rsp - so the child resumes right after the syscall,
; identical to the parent bar rax.
;
; ForkFrame layout (keep in sync with task/fork.cpp):
;   0: rbx   8: rbp  16: r12  24: r13  32: r14  40: r15
;   48: rip  56: rflags  64: rsp
;
; raw asm, not inline asm: it hardcodes rbx/rbp/r12-r15/rax, and inline asm
; might allocate another operand into one of those and clobber it. rdi is
; never written, so it stays valid as the [rdi+N] base throughout.
;
; DPL=3 selectors - same as user.cpp's enter_ring3, redeclared (asm can't see
; C++ constants).
USER_CODE_SELECTOR equ 0x20 | 3
USER_DATA_SELECTOR equ 0x18 | 3

bits 64
section .text

global fork_enter_ring3
fork_enter_ring3:
    mov rbx, [rdi + 0]
    mov rbp, [rdi + 8]
    mov r12, [rdi + 16]
    mov r13, [rdi + 24]
    mov r14, [rdi + 32]
    mov r15, [rdi + 40]

    ; IRETQ frame on our kernel stack, pushed in reverse of pop order
    ; (rip/cs/rflags/rsp/ss). rdi is still live.
    push USER_DATA_SELECTOR    ; ss
    push qword [rdi + 64]      ; rsp (parent's user rsp at syscall time)
    push qword [rdi + 56]      ; rflags (parent's, at syscall time)
    push USER_CODE_SELECTOR    ; cs
    push qword [rdi + 48]      ; rip (parent's - the instruction right after `syscall`)

    xor eax, eax                ; rax = 0 - fork()'s child-side return value

    iretq
