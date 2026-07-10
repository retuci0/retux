; low-level context switch. saves the callee-saved register set (System V
; ABI: rbx, rbp, r12-r15, plus rflags since we also toggle IF) onto the
; *current* stack, swaps RSP, then restores the same set from the *new*
; stack and returns - "returning" into whatever the new stack's return
; address points at.
;
; for a task that's run before, that's wherever it called switch_to() from
; last time (this function itself, resuming right after its own call site).
; for a brand new task, `task::create()` planted a fake frame shaped exactly
; like the one this function would have pushed, with a return address of
; `task_trampoline` - so the first switch into it "returns" straight there.
;
; void switch_to(u64* old_rsp_out, u64 new_rsp);
;   rdi = pointer to where the outgoing task's RSP should be saved
;   rsi = the incoming task's saved RSP to switch to
;
; deliberately does NOT save/restore general-purpose caller-saved registers,
; segment registers, or FPU/SSE state - the kernel is built
; `-mgeneral-regs-only` with no SSE, and every task here runs in ring 0 with
; the same segment selectors, so there's nothing there to switch.

bits 64
section .text

global switch_to
switch_to:
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

    ret                  ; pops the return address planted by task::create()
                         ; (or left there by this function's own `call`) and jumps to it
