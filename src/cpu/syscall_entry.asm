; SYSCALL entry (address in IA32_LSTAR). the CPU has switched to ring 0, put
; user RIP in RCX and RFLAGS in R11, masked RFLAGS per FMASK, and jumped here
; with RSP still on the USER stack. swapgs first, to reach the per-CPU scratch
; (and thus our kernel stack) via [gs:0]; a matching swapgs precedes sysret.
;
; Linux-style register convention:
;   rax = number, rdi/rsi/rdx = args 0-2, r10 = arg 3 (rcx is clobbered),
;   r8/r9 = args 4-5. rax = return; rcx/r11 clobbered by SYSRET.

extern syscall_dispatch

; offsets into `struct cpu::CpuLocal` (see `cpu/cpu.hpp`). must stay in
; sync with the constants there.
CPU_LOCAL_KERNEL_RSP equ 0
CPU_LOCAL_USER_RSP   equ 8

bits 64
section .text

global syscall_entry
syscall_entry:
    swapgs                             ; GS_BASE <-> IA32_KernelGSBase
    mov [gs:CPU_LOCAL_USER_RSP], rsp   ; stash user RSP
    mov rsp, [gs:CPU_LOCAL_KERNEL_RSP] ; switch to kernel stack

    ; SYSRET needs these; stash before calling C++. also become Frame fields
    ; (user_rip/user_rflags) so sys_fork() can read them back.
    push rcx                           ; user RIP
    push r11                           ; user RFLAGS
    push qword [gs:CPU_LOCAL_USER_RSP] ; user RSP (straight from memory)

    ; build a syscall::Frame, highest field first (stack grows down, struct is
    ; low-to-high). rbx/rbp/r12-r15 are callee-saved and untouched by any
    ; syscall, but sys_fork() needs the full snapshot and this is the only
    ; place it's capturable as data.
    push r15
    push r14
    push r13
    push r12
    push rbp
    push rbx
    push r9                            ; arg5
    push r8                            ; arg4
    push r10                           ; arg3 (Linux uses r10; rcx is clobbered)
    push rdx                           ; arg2
    push rsi                           ; arg1
    push rdi                           ; arg0
    push rax                           ; syscall number
    mov rdi, rsp                       ; syscall_dispatch(Frame*)

    call syscall_dispatch              ; returns u64 in rax

    ; a syscall preserves every register but rax (return), rcx and r11
    ; (eaten by SYSCALL/SYSRET). syscall_dispatch, being C++, clobbers the
    ; arg regs freely, so restore them from the Frame - musl keeps live
    ; values across syscalls (e.g. fstatat's `struct stat*` in r8).
    add rsp, 8                         ; skip saved num - rax holds the return
    pop rdi
    pop rsi
    pop rdx
    pop r10
    pop r8
    pop r9
    pop rbx                            ; rbx..r15 already intact (callee-saved);
    pop rbp                            ; restoring the copies is just uniform
    pop r12
    pop r13
    pop r14
    pop r15
    add rsp, 8                         ; discard user_rsp (SYSRET needs neither)
    pop r11                            ; user RFLAGS for SYSRET
    pop rcx                            ; user RIP for SYSRET

    mov rsp, [gs:CPU_LOCAL_USER_RSP]   ; back to user stack
    swapgs                             ; put user GS_BASE back

    o64 sysret                         ; SYSRETQ - "o64" tells NASM to emit
                                       ; the REX.W-prefixed 64-bit form.
