; SYSCALL entry point, address of which lives in `IA32_LSTAR` after
; `syscall::init()` runs. userspace executes `syscall`, the CPU switches
; CS/SS out of ring 3 into ring 0 (per STAR), saves the user RIP into RCX
; and user RFLAGS into R11, masks RFLAGS per FMASK (which we set to clear
; IF and DF), and jumps here - with RSP still pointing at the USER stack.
;
; the very first thing we do is `swapgs` to bring the kernel's per-CPU
; scratch area into GS_BASE, so we can find our own kernel stack via
; `[gs:0]`. reciprocally, everything up to the matching `swapgs` right
; before `sysret` runs with GS pointing at kernel data.
;
; register calling convention (from userspace, LINUX-style so `user_test.asm`
; is easy to write):
;   rax          = syscall number
;   rdi/rsi/rdx  = args 0-2
;   r10          = arg 3   (NOT rcx - SYSCALL clobbered rcx)
;   r8/r9        = args 4-5
;
; on return, `rax` = return value. rcx/r11 are also clobbered (SYSRET
; restores rip from rcx and rflags from r11) - userspace must treat them
; as scratch across a syscall.

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

    ; SYSRET will need these two - stash them before we call into C++
    ; (which is free to clobber all caller-saved regs).
    push rcx                           ; user RIP (SYSCALL put it here)
    push r11                           ; user RFLAGS (SYSCALL put it here)

    ; build a `syscall::Frame` on the kernel stack, in reverse of the
    ; struct's field order (highest field pushed first, since the stack
    ; grows downward and the struct is laid out low-address-to-high).
    push r9                            ; arg5
    push r8                            ; arg4
    push r10                           ; arg3  (was arg3 in userspace, would
                                       ;        have been arg3 in SysV too if
                                       ;        rcx weren't clobbered - which
                                       ;        is why linux picked r10 here)
    push rdx                           ; arg2
    push rsi                           ; arg1
    push rdi                           ; arg0
    push rax                           ; syscall number
    mov rdi, rsp                       ; syscall_dispatch(Frame*)

    call syscall_dispatch              ; returns u64 in rax

    add rsp, 7 * 8                     ; discard Frame; return value stays in rax
    pop r11                            ; restore user RFLAGS for SYSRET
    pop rcx                            ; restore user RIP for SYSRET

    mov rsp, [gs:CPU_LOCAL_USER_RSP]   ; back to user stack
    swapgs                             ; put user GS_BASE back

    o64 sysret                         ; SYSRETQ - "o64" tells NASM to emit
                                       ; the REX.W-prefixed 64-bit form.
