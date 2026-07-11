; low-level context switch. saves callee-saved regs (rbx, rbp, r12-r15, +
; rflags since we toggle IF) on the current stack, swaps RSP, restores them
; from the new stack, and rets into whatever return address sits there - the
; last switch_to() call site for a resuming task, or the task_trampoline frame
; task::create() planted for a brand-new one.
;
; void switch_to(u64* old_rsp_out, u64 new_rsp, u8* old_fpu, u8* new_fpu);
;   rdi = where to save the outgoing RSP
;   rsi = the incoming RSP to switch to
;   rdx/rcx = outgoing/incoming 16-aligned FXSAVE areas (Task::fpu_state)
;
; FXSAVE state can't be skipped like GP regs: ring-3 (musl) uses SSE2 and
; several ring-3 tasks are schedulable at once (kernel code never touches XMM,
; so it's free for kernel-only tasks). fxrstor sits before ret so it applies
; to both a resume and a first run - task::create() seeds fpu_state from
; fpu::default_state(), so it's never garbage.

bits 64
section .text

global switch_to
switch_to:
    fxsave [rdx]

    pushfq
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    mov [rdi], rsp      ; save outgoing RSP through the caller-supplied pointer
    mov rsp, rsi         ; switch to the incoming task's stack

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    popfq

    fxrstor [rcx]

    ret                  ; into task_trampoline (new task) or the last call site (resume)
