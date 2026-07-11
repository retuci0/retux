#pragma once

#include "task/task.hpp"

#include "lib/types.hpp"


// round-robin preemptive scheduler on top of `task::`.
//
// tasks live on one intrusive circular list (Task::next), always holding at
// least the "boot" task (whatever called sched::init(), normally kernel_main)
// and an idle task. the active timer driver calls tick() every tick; the
// actual switch happens from irq::set_post_eoi_hook() AFTER EOI, so a task
// suspended mid-timeslice never blocks the next interrupt.

namespace sched {

    // call once, after heap + irq are ready, from the context that becomes the
    // "boot" task. also spawns the idle task.
    void init();

    // create a task and splice it into the ring after the current one. cr3
    // defaults to 0 (no private address space) - see task::create().
    task::Task* spawn(const char* name, task::EntryFn entry, void* arg,
                       u64 stack_size = 16 * 1024, u64 cr3 = 0);

    // give up the rest of the current timeslice.
    void yield();

    // yield() from a blocking SYSCALL handler, which hasn't reached its paired
    // swapgs-exit yet. ordinary syscalls pair entry+exit swapgs before anything
    // else runs; blocking breaks that - the next task's own syscall_entry
    // swapgs would pick up our leftover KernelGSBase instead of &g_cpu_local,
    // and its [gs:...] accesses would resolve against garbage. so this pins
    // both swapgs slots to &g_cpu_local (wrmsr) around the yield.
    //
    // does NOT save/restore cpu::CpuLocal::user_rsp, which has the same
    // clobbered-by-nested-syscall problem: a caller that loops must save it
    // before the first call and restore after the last (see wait4).
    void yield_blocking();

    // called once per tick from the active timer's IRQ handler. only bumps the
    // timeslice counter + flag; does NOT switch (runs before EOI).
    void tick();

    // called from irq::set_post_eoi_hook() after EOI. does the switch if tick()
    // flagged the timeslice as expired.
    void maybe_reschedule();

    // mark the current task dead and switch away for good. an unparented task
    // is auto-reaped on a later schedule() running on a different stack; a
    // parented (forked) one stays Dead on the ring until wait4() collects it.
    // Task::exit_code should already be set (see sys_exit).
    [[noreturn]] void exit_current();

    // task with the given id anywhere on the ring (any state), or null. ids
    // are monotonic and never reused, so no ABA hazard.
    task::Task* find_task(u64 id);

    // wait for a specific child (by pid) to exit, reap it, return its pid, or
    // -ECHILD if pid isn't a live child. writes a Linux wait status to *status
    // if non-null (4-byte int, not 8 - a u64 would overflow the caller): low
    // byte 0, exit code in bits 8-15. busy-yield()s (never a bare spin - IF is
    // clear all syscall, so only switch_to lets the child run). one specific
    // pid only - no -1, no WNOHANG.
    //
    // limitation: a child whose parent exits first becomes a permanent zombie
    // (no orphan re-parenting). retsh's fork-then-wait never hits this.
    i64 wait4(i64 pid, u32* status);

    // the task currently executing.
    task::Task* current_task();

}
