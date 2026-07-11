#pragma once

#include "lib/types.hpp"


// SYSCALL/SYSRET syscall entry. requires the GDT per boot.asm (for STAR's
// selector pair) and cpu::init() already run (the entry stub swapgs'es on its
// first instruction and would trap on an uninitialized IA32_KernelGSBase).
// number in rax, args in rdi/rsi/rdx/r10/r8/r9 - the real Linux ABI.

namespace syscall {

    // real x86-64 Linux syscall numbers (arch/x86/entry/syscalls/
    // syscall_64.tbl) - the goal is running unmodified static musl binaries.
    // only the implemented ones are listed; the rest hit syscall_dispatch's
    // default, which logs and returns -ENOSYS.
    constexpr u64 SYS_READ              = 0;
    constexpr u64 SYS_WRITE             = 1;
    constexpr u64 SYS_OPEN              = 2;  // legacy non-`at` form; musl uses it here
    constexpr u64 SYS_CLOSE             = 3;
    constexpr u64 SYS_STAT              = 4;  // legacy path-based - see sys_stat
    constexpr u64 SYS_FSTAT             = 5;
    constexpr u64 SYS_LSEEK             = 8;
    constexpr u64 SYS_MMAP              = 9;
    constexpr u64 SYS_MPROTECT          = 10;
    constexpr u64 SYS_MUNMAP            = 11;
    constexpr u64 SYS_BRK               = 12;
    constexpr u64 SYS_RT_SIGACTION      = 13;
    constexpr u64 SYS_RT_SIGPROCMASK    = 14;
    constexpr u64 SYS_IOCTL             = 16;
    constexpr u64 SYS_WRITEV            = 20;
    constexpr u64 SYS_ACCESS            = 21;  // legacy path-based - see sys_access
    constexpr u64 SYS_GETCWD            = 79;
    constexpr u64 SYS_READLINK          = 89;
    constexpr u64 SYS_FORK              = 57;
    constexpr u64 SYS_EXECVE            = 59;
    constexpr u64 SYS_EXIT              = 60;
    constexpr u64 SYS_WAIT4             = 61;
    constexpr u64 SYS_GETPID            = 39;
    constexpr u64 SYS_UNAME             = 63;
    constexpr u64 SYS_GETUID            = 102;
    constexpr u64 SYS_GETGID            = 104;
    constexpr u64 SYS_GETEUID           = 107;
    constexpr u64 SYS_GETEGID           = 108;
    constexpr u64 SYS_GETPPID           = 110;
    constexpr u64 SYS_ARCH_PRCTL        = 158;
    constexpr u64 SYS_SET_TID_ADDRESS   = 218;
    constexpr u64 SYS_CLOCK_GETTIME     = 228;
    constexpr u64 SYS_EXIT_GROUP        = 231;
    constexpr u64 SYS_GETDENTS64        = 217;
    constexpr u64 SYS_OPENAT            = 257;
    constexpr u64 SYS_SET_ROBUST_LIST   = 273;
    constexpr u64 SYS_GETRANDOM         = 318;

    // built by the entry stub, low-to-high in push order. the callee-saved
    // regs and user_rsp/rflags/rip exist only so sys_fork() can snapshot the
    // caller's full register state - it reads this saved copy, valid even
    // after syscall_dispatch has clobbered the live registers.
    struct Frame {
        u64 num;          // was rax (syscall number)
        u64 arg0;         // was rdi
        u64 arg1;         // was rsi
        u64 arg2;         // was rdx
        u64 arg3;         // was r10 (not rcx - SYSCALL clobbers rcx)
        u64 arg4;         // was r8
        u64 arg5;         // was r9
        u64 rbx;          // callee-saved, captured only for sys_fork
        u64 rbp;
        u64 r12;
        u64 r13;
        u64 r14;
        u64 r15;
        u64 user_rsp;     // user RSP at the syscall
        u64 user_rflags;  // was r11
        u64 user_rip;     // was rcx (return rip)
    };

    // program EFER.SCE / STAR / LSTAR / FMASK. call once, after cpu::init().
    void init();

}
