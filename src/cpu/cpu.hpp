#pragma once

#include "lib/types.hpp"


// tiny per-CPU scratch area that the syscall entry stub reads via `swapgs`
// + `[gs:offset]`. right now the whole kernel is single-CPU, so there is
// exactly one instance, addressed via `IA32_KernelGSBase` (0xC000_0102) -
// which SYSCALL doesn't touch directly but which `swapgs` exchanges with
// `GS_BASE`, so a `swapgs` right at the top of the SYSCALL entry point
// makes `[gs:0]` refer to fields of this struct instead of whatever
// nonsense value userspace had in GS at the time of the syscall.
//
// field offsets are hardcoded in `syscall_entry.asm` - do NOT reorder
// without updating both places in lockstep.

namespace cpu {

    struct CpuLocal {
        u64 kernel_rsp;  // offset 0  - top of the currently-running task's kernel stack.
                         //             kept in sync by `sched::schedule()` on every switch.
        u64 user_rsp;    // offset 8  - scratch: syscall entry stashes user RSP here
                         //             before switching to `kernel_rsp` above.
    };

    static constexpr u32 CPU_LOCAL_KERNEL_RSP_OFFSET = 0;
    static constexpr u32 CPU_LOCAL_USER_RSP_OFFSET   = 8;

    // program `IA32_KernelGSBase` to point at the (single) CpuLocal. call
    // once, from `kernel_main`, BEFORE `syscall::init()` - the syscall
    // entry stub's very first instruction is `swapgs`, and that swap is
    // only meaningful if this MSR has been programmed already.
    void init();

    // pointer to the singleton, for `sched::schedule()` to poke at.
    CpuLocal* local();

}
