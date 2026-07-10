#pragma once

#include "task/task.hpp"

#include "lib/types.hpp"


// round-robin preemptive scheduler on top of `task::`.
//
// tasks live on a single intrusive circular linked list (`Task::next`) that
// always contains at least the bootstrap task ("boot" - whichever context
// called `sched::init()`, normally `kernel_main`) and a dedicated idle task.
// preemption is driven by whichever timer driver is active (`pit`/`hpet`)
// calling `sched::tick()` on every tick via `set_tick_callback()`, and the
// actual switch happens from `irq::set_post_eoi_hook()` - AFTER EOI has
// already been sent, so a task that ends up suspended mid-timeslice never
// blocks the next interrupt on that vector.

namespace sched {

    // must be called once, after heap + irq are ready, from the context
    // that should become the "boot" task (normally `kernel_main`, right
    // before it does anything it wants preemptible). also spawns a dedicated
    // idle task (halts when nothing else is ready).
    void init();

    // create a new task and add it to the ready queue, right after the
    // currently running one.
    task::Task* spawn(const char* name, task::EntryFn entry, void* arg,
                       u64 stack_size = 16 * 1024);

    // voluntarily give up the remainder of the current timeslice.
    void yield();

    // called from the active timer driver's IRQ handler (via
    // `pit::set_tick_callback` / `hpet::set_tick_callback`) once per tick.
    // only decrements the timeslice counter and sets a flag - does NOT
    // itself switch stacks, since it runs before EOI has been sent.
    void tick();

    // called from `irq::set_post_eoi_hook()` after every IRQ, once EOI has
    // already been sent. actually performs the switch if `tick()` flagged
    // that the current task's timeslice ran out.
    void maybe_reschedule();

    // marks the current task dead and switches away from it permanently.
    // its stack is reclaimed (via `heap::kfree`) the next time `schedule()`
    // runs on a *different* stack - never while still executing on it.
    [[noreturn]] void exit_current();

    // the task currently executing.
    task::Task* current_task();

}
