#pragma once

#include "lib/types.hpp"


namespace tss {

    void init();

    // update RSP0 - the stack the CPU loads on any ring3->ring0 transition
    // that goes through a normal (non-IST) interrupt gate, e.g. a timer IRQ
    // landing while ring-3 code is executing (SYSCALL doesn't use this - it
    // has its own swapgs + cpu_local.kernel_rsp mechanism, see cpu/syscall_
    // entry.asm - but a hardware IRQ arriving via the IDT does). must be
    // kept in sync with whichever task is about to run, same as
    // cpu_local.kernel_rsp - see `sched.cpp`'s `schedule()`, right next to
    // that update. left pointing at the small double-fault-only stack
    // (`tss::init()`'s default) would otherwise have ANY interrupt-during-
    // ring-3 clobber whatever task's Task::rsp gets saved into it.
    void set_kernel_stack(u64 rsp0);

}