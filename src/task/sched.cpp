#include "task/sched.hpp"
#include "task/task.hpp"

#include "cpu/cpu.hpp"
#include "cpu/tss.hpp"
#include "cpu/fpu.hpp"

#include "mem/heap.hpp"
#include "mem/vmm.hpp"

#include "lib/types.hpp"
#include "lib/string.hpp"
#include "lib/errno.hpp"

#include "io/serial.hpp"


// defined in switch.asm. saves callee-saved regs + rflags + FXSAVE (into
// old_fpu) on the outgoing stack, stashes RSP through old_rsp_out, swaps to
// new_rsp and restores the same from new_fpu. handles both a resume and a new
// task's first run (fxrstor sits right before the ret).
extern "C" void switch_to(u64* old_rsp_out, u64 new_rsp, u8* old_fpu, u8* new_fpu);

using task::State;
using task::Task;

namespace {

    // ticks per task before preemption. 20ms at the default 1000 Hz.
    constexpr u32 TIMESLICE_TICKS = 20;

    Task* current_   = nullptr;
    Task* idle_task_ = nullptr;

    // set by exit_current() before the final switch away from a dying task;
    // reaped on a later schedule() running on a *different* stack (freeing it
    // sooner would pull the rug from under the stack still executing).
    Task* zombie_ = nullptr;

    volatile u32  ticks_left_   = TIMESLICE_TICKS;
    volatile bool need_resched_ = false;

    // unlink t from the ring. t must not be current_ (can't unlink the stack
    // we're on); both call sites guarantee that.
    void unlink_from_ring(Task* t) {
        Task* prev = current_;
        while (prev->next != t) prev = prev->next;
        prev->next = t->next;
    }

    // frees everything a dead task owns: address space, kernel stack, FXSAVE
    // area, Task struct. caller must have unlinked t and must not be running
    // on t's stack/CR3 - reaping only happens once something else is current_.
    void free_task(Task* t) {
        if (t->cr3 != 0) vmm::destroy_address_space(t->cr3);
        heap::kfree(reinterpret_cast<void*>(t->stack_base));
        heap::kfree(t->fpu_raw);
        heap::kfree(t);
    }

    void reap_zombie() {
        if (!zombie_) return;
        // exit_current() sets zombie_ = current_ before switching away, so the
        // immediately-following schedule() still sees zombie_ == current_ -
        // don't free the stack we're on. a later schedule() reaps it.
        if (zombie_ == current_) return;

        unlink_from_ring(zombie_);
        free_task(zombie_);
        zombie_ = nullptr;
    }

    // first Ready task strictly after `from` on the ring, or null if none.
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

    // pick a task and (maybe) switch to it. safe from both yield() (voluntary)
    // and maybe_reschedule() (post-EOI IRQ) - both normal kernel-stack contexts.
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

        // point SYSCALL's entry stack (cpu_local) and the IDT's non-IST gate
        // stack (TSS.RSP0) at the incoming task's kernel stack. both matter:
        // SYSCALL uses cpu_local, a hardware IRQ landing in ring 3 uses RSP0.
        // leaving RSP0 unsynced would land an interrupt on the wrong task's
        // stack and corrupt its saved rsp. 0 = boot task, no kernel stack.
        if (next->kernel_stack_top != 0) {
            cpu::local()->kernel_rsp = next->kernel_stack_top;
            tss::set_kernel_stack(next->kernel_stack_top);
        }

        // restore the incoming task's TLS (arch_prctl ARCH_SET_FS). no-op for
        // kernel-only tasks (fs_base stays 0).
        cpu::wrmsr(cpu::IA32_FS_BASE, next->fs_base);

        // load the task's address space if it has one. 0 = kernel-only task -
        // leave CR3 alone; those only touch shared mappings, so switching
        // would just be a needless TLB flush.
        if (next->cr3 != 0) {
            asm volatile("mov %0, %%cr3" : : "r"(next->cr3) : "memory");
        }

        switch_to(&old->rsp, next->rsp, old->fpu_state, next->fpu_state);
        // resumes here next time something switches back to `old`.
    }

}

// where a freshly-created task "returns" on its first switch-in, instead of a
// real call site - see task::create()'s InitialFrame setup.
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
        // wrap the calling context (kernel_main) as the "boot" task. it has no
        // manufactured stack - its rsp is only written the first time
        // switch_to() switches away from it.
        Task* boot = static_cast<Task*>(heap::kmalloc(sizeof(Task)));
        boot->rsp              = 0;
        boot->stack_base       = 0;  // not owned by us - never freed
        boot->stack_size       = 0;
        boot->kernel_stack_top = 0;  // boot task never takes syscalls
        boot->id               = 0;
        boot->state            = State::Running;
        boot->entry            = nullptr;
        boot->arg              = nullptr;
        boot->fs_base   = 0;
        boot->cr3       = 0;
        boot->brk_start = 0;
        boot->brk_cur   = 0;
        boot->mmap_next = 0;
        // boot skips task::create(), so it repeats the 16-aligned FXSAVE-area
        // setup by hand.
        u8* boot_fpu_raw = static_cast<u8*>(heap::kmalloc(512 + 15));
        boot->fpu_raw   = boot_fpu_raw;
        boot->fpu_state = reinterpret_cast<u8*>((reinterpret_cast<u64>(boot_fpu_raw) + 15) & ~15ULL);
        string::memcpy(boot->fpu_state, fpu::default_state(), 512);
        boot->parent    = nullptr;
        boot->exit_code = 0;
        for (u32 i = 0; i < Task::MAX_FDS; ++i) boot->fds[i] = nullptr;
        boot->name[0] = 'b'; boot->name[1] = 'o'; boot->name[2] = 'o';
        boot->name[3] = 't'; boot->name[4] = '\0';

        idle_task_ = task::create("idle", idle_entry, nullptr, 4096);

        // splice into a two-element ring: boot -> idle -> boot
        boot->next       = idle_task_;
        idle_task_->next = boot;

        current_ = boot;

        serial::print("sched: ready (boot + idle tasks)\n");
    }

    Task* spawn(const char* name, task::EntryFn entry, void* arg, u64 stack_size, u64 cr3) {
        Task* t = task::create(name, entry, arg, stack_size, cr3);
        if (!t) return nullptr;

        // splice in right after the current task.
        t->next = current_->next;
        current_->next = t;
        return t;
    }

    void yield() {
        schedule();
    }

    void yield_blocking() {
        cpu::wrmsr(cpu::IA32_KERNEL_GS_BASE, reinterpret_cast<u64>(cpu::local()));
        yield();
        cpu::wrmsr(cpu::IA32_GS_BASE, reinterpret_cast<u64>(cpu::local()));
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
        current_->state = State::Dead;

        // only unparented tasks auto-reap (schedule() reaps the previous
        // zombie first). a parented task sits Dead on the ring for wait4() -
        // reaping it here would use-after-free if wait4 lost a race to any
        // other task getting scheduled first (see task.hpp's Task::parent).
        if (current_->parent == nullptr) {
            zombie_ = current_;
        }
        schedule();

        while (true) asm volatile("hlt");  // unreachable; [[noreturn]] needs a trap
    }

    Task* find_task(u64 id) {
        Task* t = current_;
        do {
            if (t->id == id) return t;
            t = t->next;
        } while (t != current_);
        return nullptr;
    }

    i64 wait4(i64 pid, u32* status) {
        Task* self = current_;
        Task* child = find_task(static_cast<u64>(pid));
        if (!child || child->parent != self) return -err::ECHILD;

        // save/restore user_rsp around the whole blocking loop - see
        // yield_blocking()'s doc comment for the swapgs hazard.
        u64 saved_user_rsp = cpu::local()->user_rsp;
        while (child->state != State::Dead) {
            yield_blocking();
        }
        cpu::local()->user_rsp = saved_user_rsp;

        // status = exit code in bits 8-15, low byte 0 (WIFEXITED). no signals,
        // so never a signal-death status.
        if (status) *status = static_cast<u32>((child->exit_code & 0xFF) << 8);
        i64 pid_out = static_cast<i64>(child->id);
        unlink_from_ring(child);
        free_task(child);
        return pid_out;
    }

    Task* current_task() {
        return current_;
    }

}
