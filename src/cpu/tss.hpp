#pragma once

#include "lib/types.hpp"


namespace tss {

    void init();

    // update RSP0 - the stack the CPU loads on a ring3->ring0 transition
    // through a non-IST interrupt gate (a hardware IRQ during ring 3; SYSCALL
    // uses cpu_local.kernel_rsp instead). schedule() keeps it synced to the
    // running task, alongside cpu_local.kernel_rsp.
    void set_kernel_stack(u64 rsp0);

}