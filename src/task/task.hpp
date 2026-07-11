#pragma once

#include "fs/vfs.hpp"

#include "lib/types.hpp"


// task control block + creation. kernel-only tasks share whatever CR3 is
// active; a Linux-ABI task gets its own via the `cr3` field (see
// vmm::create_address_space(), task/user.cpp).

namespace task {

    using EntryFn = void (*)(void* arg);

    enum class State : u8 {
        Ready,    // in the run queue, not currently executing
        Running,  // currently executing on the CPU
        Dead,     // entry function returned / exited - pending `sched::reap()`
    };

    struct Task {
        // saved stack pointer, read/written by switch_to() - only meaningful
        // while this task isn't running.
        u64 rsp;

        u64 stack_base;  // base of the kmalloc'd stack, freed on exit
        u64 stack_size;

        // top of the kernel stack (16-aligned), loaded into cpu_local.kernel_rsp
        // + TSS.RSP0 when picked so a SYSCALL/IRQ lands on THIS task's stack.
        // 0 for the boot task (no kmalloc'd stack, never takes syscalls).
        u64 kernel_stack_top;

        u64   id;
        State state;

        EntryFn entry;
        void*   arg;

        // --- Linux-ABI process state (task/user.cpp, cpu/syscall.cpp) ---
        // all zeroed by task::create(), not default member initializers - Task
        // is kmalloc'd + cast, never constructed, so those wouldn't run.

        u64 fs_base;     // TLS pointer (arch_prctl ARCH_SET_FS), restored into
                         // IA32_FS_BASE on every switch in so it doesn't leak.

        u64 cr3;         // this task's own PML4, loaded on switch in; 0 for
                         // kernel-only tasks (share the active CR3).

        u64 brk_start;  // brk region [brk_start, brk_cur), grown by sys_brk.
        u64 brk_cur;    // brk_start fixed at ELF load (past the last PT_LOAD).

        u64 mmap_next;  // anonymous-mmap bump pointer, separate from brk.

        // per-task FXSAVE area (512 bytes, 16-aligned for fxsave/fxrstor).
        // fpu_raw is the kmalloc'd block (for kfree); fpu_state is it rounded
        // up to 16. seeded from fpu::default_state() - an all-zero image
        // unmasks every FP exception and would fault on first use.
        u8* fpu_raw;
        u8* fpu_state;

        // set by sys_fork(), null otherwise. exit_current() auto-reaps a dead
        // task only when parent == nullptr; a parented one lingers for
        // sys_wait4, which collects exit_code.
        Task* parent;
        u64   exit_code;

        static constexpr u32 MAX_FDS = 16;
        vfs::File* fds[MAX_FDS];  // fd 0/1/2 special-cased (tty/serial), no File

        Task* next;  // intrusive circular ready queue - see sched.cpp

        char name[16];
    };

    // allocate a task + kernel stack with a fake initial switch_to() frame, so
    // its first switch-in "returns" into task_trampoline (which calls
    // entry(arg), then exit_current()). does NOT add it to the ready queue -
    // use sched::spawn() (this is standalone only for testing).
    //
    // cr3 is set during construction, not afterward: spawn() splices the task
    // into the ring immediately, and a timer could preempt into it before a
    // separate "set cr3" step, briefly running it with no address space.
    Task* create(const char* name, EntryFn entry, void* arg,
                 u64 stack_size = 16 * 1024, u64 cr3 = 0);

}
