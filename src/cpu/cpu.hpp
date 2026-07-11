#pragma once

#include "lib/types.hpp"


// per-CPU scratch the syscall entry stub reaches via swapgs + [gs:offset].
// single-CPU, so one instance, pointed at by IA32_KernelGSBase; the entry
// stub's opening swapgs makes [gs:0] refer to this struct rather than
// userspace's GS.
//
// field offsets are hardcoded in syscall_entry.asm - don't reorder without
// updating both.

namespace cpu {

    struct CpuLocal {
        u64 kernel_rsp;  // top of the running task's kernel stack (synced by schedule())
        u64 user_rsp;    // scratch: entry stashes user RSP here before switching
    };

    static constexpr u32 CPU_LOCAL_KERNEL_RSP_OFFSET = 0;
    static constexpr u32 CPU_LOCAL_USER_RSP_OFFSET   = 8;

    // point IA32_KernelGSBase at the CpuLocal. call once, before syscall::init()
    // - the entry stub's first instruction is swapgs, meaningless until this
    // MSR is set.
    void init();

    // pointer to the singleton, for schedule().
    CpuLocal* local();

    // TLS base - arch_prctl(ARCH_SET_FS) writes it, schedule() restores it
    // per-task from Task::fs_base.
    constexpr u32 IA32_FS_BASE = 0xC000'0100;

    // the two swapgs-exchanged MSRs. exposed because wait4() writes them
    // directly (wrmsr, not swapgs) - once more than one task can be mid-syscall,
    // swapgs's "restore whatever was there" isn't safe; see yield_blocking().
    constexpr u32 IA32_GS_BASE        = 0xC000'0101;
    constexpr u32 IA32_KERNEL_GS_BASE = 0xC000'0102;

    u64  rdmsr(u32 msr);
    void wrmsr(u32 msr, u64 value);

}
