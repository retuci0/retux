#pragma once

#include "lib/types.hpp"


// SYSCALL/SYSRET-based syscall entry. requires:
//   - GDT laid out per boot.asm's comment (kernel_code=0x08, user_data=0x18,
//     user_code=0x20) so STAR can encode the right selector pair.
//   - cpu::init() already called - the entry stub `swapgs`es on the very
//     first cycle and would trap on an uninitialized IA32_KernelGSBase.
//
// syscall arguments follow the Linux/System V convention (mostly - we
// pass the number in rax, and args in rdi, rsi, rdx, r10, r8, r9), only
// because it's the calling convention userspace test code is easiest to
// write against. no promise of ABI stability - this is a hobby OS.

namespace syscall {

    // real x86-64 Linux syscall numbers (see Linux's arch/x86/entry/syscalls/
    // syscall_64.tbl) - this used to be "deliberately overlapping, no ABI
    // commitment" but the goal now IS Linux ABI compatibility (running real
    // static/non-PIE musl binaries unmodified), so these are the actual
    // numbers, not lookalikes. only the ones retux implements are listed;
    // anything else falls through `syscall_dispatch`'s default case, which
    // logs the number and returns -ENOSYS instead of guessing.
    constexpr u64 SYS_READ              = 0;
    constexpr u64 SYS_WRITE             = 1;
    constexpr u64 SYS_OPEN              = 2;  // x86-64 still has the legacy
                                               // non-`at` form - musl's
                                               // open() uses it directly
                                               // on this arch.
    constexpr u64 SYS_CLOSE             = 3;
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
    constexpr u64 SYS_GETPID            = 39;
    constexpr u64 SYS_EXIT              = 60;
    constexpr u64 SYS_UNAME             = 63;
    constexpr u64 SYS_GETUID            = 102;
    constexpr u64 SYS_GETGID            = 104;
    constexpr u64 SYS_GETEUID           = 107;
    constexpr u64 SYS_GETEGID           = 108;
    constexpr u64 SYS_ARCH_PRCTL        = 158;
    constexpr u64 SYS_SET_TID_ADDRESS   = 218;
    constexpr u64 SYS_CLOCK_GETTIME     = 228;
    constexpr u64 SYS_EXIT_GROUP        = 231;
    constexpr u64 SYS_OPENAT            = 257;
    constexpr u64 SYS_SET_ROBUST_LIST   = 273;
    constexpr u64 SYS_GETRANDOM         = 318;

    // captured verbatim from the SYSCALL entry stub, low address to high
    // matches the order fields are pushed there.
    struct Frame {
        u64 num;   // syscall number (was rax on entry)
        u64 arg0;  // was rdi
        u64 arg1;  // was rsi
        u64 arg2;  // was rdx
        u64 arg3;  // was r10 - note: NOT rcx, since SYSCALL clobbers rcx
        u64 arg4;  // was r8
        u64 arg5;  // was r9
    };

    // program EFER.SCE / STAR / LSTAR / FMASK. call once, from kernel_main,
    // AFTER cpu::init().
    void init();

}
