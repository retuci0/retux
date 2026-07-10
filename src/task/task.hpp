#pragma once

#include "lib/types.hpp"


// task control block + task creation. this is deliberately dumb: kernel-mode
// only threads (no address-space switch, no ring 3) sharing the single set
// of page tables set up by `vmm::remap_kernel()`. userspace tasks are a
// later phase (ELF loader + syscalls) that will extend `Task` with a CR3.

namespace task {

    using EntryFn = void (*)(void* arg);

    enum class State : u8 {
        Ready,    // in the run queue, not currently executing
        Running,  // currently executing on the CPU
        Dead,     // entry function returned / exited - pending `sched::reap()`
    };

    struct Task {
        // saved stack pointer. only meaningful while this task is NOT the
        // one currently running - `switch_to()` writes/reads it directly.
        u64 rsp;

        u64 stack_base;  // base of the kmalloc'd stack, for freeing on exit
        u64 stack_size;

        // stack_base + stack_size, rounded down to 16-byte alignment - the
        // value the scheduler writes into `cpu_local.kernel_rsp` when this
        // task is picked, so that any subsequent SYSCALL from userspace
        // lands on THIS task's kernel stack rather than whoever ran last.
        // 0 for the boot task (see `sched::init()`), which has no
        // kmalloc'd stack and never itself takes syscalls.
        u64 kernel_stack_top;

        u64   id;
        State state;

        EntryFn entry;
        void*   arg;

        // intrusive singly-linked circular ready queue - see `sched.cpp`.
        Task* next;

        char name[16];
    };

    // allocate a task and its kernel stack, and lay out a fake initial
    // `switch_to()` frame on it so that switching into it for the first
    // time behaves exactly like resuming a task that yielded normally -
    // it "returns" straight into `task_trampoline`, which calls `entry(arg)`
    // and then `sched::exit_current()` once it returns.
    //
    // does NOT add the task to the scheduler's ready queue - use
    // `sched::spawn()` for that (this only exists standalone for testing).
    Task* create(const char* name, EntryFn entry, void* arg, u64 stack_size = 16 * 1024);

}
