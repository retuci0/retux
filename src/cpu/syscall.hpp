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

    // syscall numbers matching what `src/task/user_test.asm` expects.
    // deliberately picked to overlap with Linux's values (`SYS_write=1`,
    // `SYS_exit=60`) so it's easy to compare disassembly at a glance -
    // this is not an ABI commitment.
    constexpr u64 SYS_WRITE = 1;
    constexpr u64 SYS_EXIT  = 60;

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
