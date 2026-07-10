#pragma once

#include "fs/vfs.hpp"

#include "lib/types.hpp"


// task control block + task creation. this is deliberately dumb: every task,
// kernel or ring-3, shares the single set of page tables set up by
// `vmm::remap_kernel()` - there's no per-task CR3 yet, so `task::user`
// hand-maps ring-3 code/stack pages into that same shared address space
// (see `task/user.cpp`, `task/elf.cpp`) rather than switching into an
// isolated one. real process isolation is later work that would extend
// `Task` with a CR3 and give `sched::spawn()` an address-space argument.

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

        // --- Linux-ABI process state (task/user.cpp, cpu/syscall.cpp) ---
        // all zeroed by `task::create()` below - NOT via default member
        // initializers, since every Task is `kmalloc`'d + cast rather than
        // constructed, so those would never actually run.

        u64 fs_base;     // TLS pointer, set via arch_prctl(ARCH_SET_FS) -
                         // restored into IA32_FS_BASE on every switch INTO
                         // this task (see `sched.cpp`'s `schedule()`) so a
                         // task's TLS doesn't leak into the next one.

        u64 cr3;         // physical address of this task's own PML4 (see
                         // `vmm::create_address_space()`), loaded on every
                         // switch INTO this task - 0 for kernel-only tasks
                         // (boot/idle/kbdecho), which share whatever CR3 was
                         // already active rather than owning one (same
                         // "doesn't apply to this task" convention as
                         // `kernel_stack_top == 0`).

        u64 brk_start;  // user `brk` region: [brk_start, brk_cur), grown
        u64 brk_cur;    // page-at-a-time by `sys_brk`. brk_start is fixed at
                        // ELF-load time (just past the last PT_LOAD).

        u64 mmap_next;  // bump pointer for anonymous mmap - separate region
                        // from brk so the two never collide.

        static constexpr u32 MAX_FDS = 16;
        vfs::File* fds[MAX_FDS];  // fd 0/1/2 are special-cased (tty/serial)
                                  // in the syscall handlers, not backed by
                                  // a real vfs::File.

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
    //
    // `cr3` (default 0, "no private address space") is set on the Task
    // struct atomically as part of construction, not by the caller
    // afterward - a task is spliced into the ready queue as soon as
    // `sched::spawn()` returns, and a timer interrupt could preempt into it
    // before a separate "now set its cr3" step ever ran, briefly running it
    // with no address space of its own. see `vmm::create_address_space()`.
    Task* create(const char* name, EntryFn entry, void* arg,
                 u64 stack_size = 16 * 1024, u64 cr3 = 0);

}
