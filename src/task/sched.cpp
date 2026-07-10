#include "task/sched.hpp"
#include "task/task.hpp"

#include "cpu/cpu.hpp"

#include "mem/heap.hpp"

#include "lib/types.hpp"

#include "io/serial.hpp"


// defined in `switch.asm`. saves the callee-saved register set + rflags
// onto the outgoing stack, writes the resulting RSP to `*old_rsp_out`,
// switches RSP to `new_rsp`, and restores the same register set from there.
extern "C" void switch_to(u64* old_rsp_out, u64 new_rsp);

using task::State;
using task::Task;

namespace {

    // how many ticks each task gets before being preempted. at the
    // kernel's default 1000 Hz tick rate that's a 20ms timeslice.
    constexpr u32 TIMESLICE_TICKS = 20;

    Task* current_   = nullptr;
    Task* idle_task_ = nullptr;

    // set by `exit_current()` right before the final switch away from a
    // dying task - reclaimed on the next `schedule()` call that runs on a
    // *different* stack, since freeing it any earlier would pull the rug
    // out from under whatever's still executing on it.
    Task* zombie_ = nullptr;

    volatile u32  ticks_left_   = TIMESLICE_TICKS;
    volatile bool need_resched_ = false;

    void reap_zombie() {
        if (!zombie_) return;
        // `exit_current()` sets `zombie_ = current_` *before* switching away
        // from it - if `schedule()` is being called from that same task
        // (which it always is, right after), `zombie_ == current_` still,
        // and we must not free the stack we're actively executing on. it'll
        // get reaped on some later `schedule()` call once `current_` has
        // moved on to something else.
        if (zombie_ == current_) return;
        heap::kfree(reinterpret_cast<void*>(zombie_->stack_base));
        heap::kfree(zombie_);
        zombie_ = nullptr;
    }

    // first `Ready` task strictly after `from`, walking the circular list.
    // returns nullptr if there isn't one (i.e. `from` is the only runnable
    // task on the ring).
    Task* next_ready_after(Task* from) {
        Task* t = from->next;
        while (t != from) {
            if (t->state == State::Ready) return t;
            t = t->next;
        }
        return nullptr;
    }

    void idle_entry(void*) {
        while (true) {
            asm volatile("sti; hlt");
        }
    }

    // does the actual work of picking a task and (maybe) switching to it.
    // safe to call from both `yield()` (voluntary, arbitrary C++ context)
    // and `maybe_reschedule()` (involuntary, from post-EOI IRQ context) -
    // both are "normal" kernel-stack contexts as far as `switch_to` cares.
    void schedule() {
        reap_zombie();

        Task* old = current_;
        Task* next = next_ready_after(old);

        if (!next) {
            // nobody else is ready. if `old` itself can still run, just
            // keep going - otherwise (it just called exit_current()) fall
            // back to idle, which is always present and always ready.
            next = (old->state == State::Running) ? old : idle_task_;
        }

        ticks_left_   = TIMESLICE_TICKS;
        need_resched_ = false;

        if (next == old) return;  // nothing to actually switch

        if (old->state == State::Running) old->state = State::Ready;
        next->state = State::Running;
        current_ = next;

        // point the per-CPU syscall-entry stack at the incoming task's
        // kernel stack top - so a SYSCALL from userspace (whenever this
        // task eventually SYSRETs out and comes back) lands on THIS task's
        // stack rather than whichever one happened to run last.
        // 0 = "no kernel stack" (boot task) - leave the field alone,
        // since it won't ever take a syscall to notice.
        if (next->kernel_stack_top != 0) {
            cpu::local()->kernel_rsp = next->kernel_stack_top;
        }

        switch_to(&old->rsp, next->rsp);

        // execution resumes here once something switches back to `old` -
        // which, by definition, is only ever reached again by re-entering
        // this exact call site the next time `old` gets picked.
    }

}

// every freshly-created task (via `task::create`) "returns" here the first
// time it's switched to, instead of into a real call site - see
// `task::create()`'s `InitialFrame` setup.
extern "C" void task_trampoline() {
    Task* self = sched::current_task();
    if (self && self->entry) {
        self->entry(self->arg);
    }
    sched::exit_current();
    // unreachable - exit_current() never returns
}

namespace sched {

    void init() {
        // wrap whatever context called `init()` (normally `kernel_main`) as
        // the "boot" task. it has no `task::create()`-manufactured stack -
        // its `rsp` field is only ever populated the first time something
        // switches *away* from it, by `switch_to()` itself.
        Task* boot = static_cast<Task*>(heap::kmalloc(sizeof(Task)));
        boot->rsp              = 0;
        boot->stack_base       = 0;  // not owned by us - never freed
        boot->stack_size       = 0;
        boot->kernel_stack_top = 0;  // boot task never takes syscalls
        boot->id               = 0;
        boot->state            = State::Running;
        boot->entry            = nullptr;
        boot->arg              = nullptr;
        boot->name[0] = 'b'; boot->name[1] = 'o'; boot->name[2] = 'o';
        boot->name[3] = 't'; boot->name[4] = '\0';

        idle_task_ = task::create("idle", idle_entry, nullptr, 4096);

        // splice into a two-element ring: boot -> idle -> boot
        boot->next       = idle_task_;
        idle_task_->next = boot;

        current_ = boot;

        serial::print("sched: ready (boot + idle tasks)\n");
    }

    Task* spawn(const char* name, task::EntryFn entry, void* arg, u64 stack_size) {
        Task* t = task::create(name, entry, arg, stack_size);
        if (!t) return nullptr;

        // splice in right after the current task.
        t->next = current_->next;
        current_->next = t;
        return t;
    }

    void yield() {
        schedule();
    }

    void tick() {
        if (!current_) return;  // sched::init() hasn't run yet
        if (ticks_left_ > 0 && --ticks_left_ == 0) {
            need_resched_ = true;
        }
    }

    void maybe_reschedule() {
        if (!current_ || !need_resched_) return;
        schedule();
    }

    [[noreturn]] void exit_current() {
        // `schedule()` (below) reaps whatever the *previous* zombie was as
        // its first step - no need to do it here too.
        current_->state = State::Dead;
        zombie_ = current_;
        schedule();

        // schedule() switches away from a Dead current_ and never returns
        // to a Dead task, so this is truly unreachable - but the compiler
        // can't know that, and `[[noreturn]]` demands a trap just in case.
        while (true) asm volatile("hlt");
    }

    Task* current_task() {
        return current_;
    }

}
