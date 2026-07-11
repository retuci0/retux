#include "task/user.hpp"
#include "task/sched.hpp"

#include "mem/vmm.hpp"
#include "mem/heap.hpp"

#include "lib/types.hpp"
#include "lib/errno.hpp"

#include "io/serial.hpp"


namespace {

    // register/stack snapshot sys_fork() takes from the parent's Frame.
    // fork_entry.asm indexes this by hardcoded byte offset - keep the field
    // order in sync with that file.
    struct ForkFrame {
        u64 rbx, rbp, r12, r13, r14, r15;
        u64 rip, rflags, rsp;
    };

}

// defined in `task/fork_entry.asm`. never returns - IRETQs straight into
// ring 3 with the saved register state above, forcing rax = 0.
extern "C" [[noreturn]] void fork_enter_ring3(const ForkFrame* f);

namespace {

    // entry fn for the forked child (a normal spawn'd task). the only thing
    // special vs spawn_from_elf is how it enters ring 3: fork_enter_ring3
    // resumes it mid-syscall instead of jumping to an ELF entry point.
    [[noreturn]] void fork_child_entry(void* arg_ptr) {
        ForkFrame f = *static_cast<ForkFrame*>(arg_ptr);
        heap::kfree(arg_ptr);
        fork_enter_ring3(&f);
        // unreachable - fork_enter_ring3 never returns
    }

}

namespace task::user {

    i64 sys_fork(const syscall::Frame* f) {
        task::Task* parent = sched::current_task();

        u64 child_cr3 = vmm::create_address_space();
        vmm::clone_address_space(child_cr3, parent->cr3);

        auto* saved = static_cast<ForkFrame*>(heap::kmalloc(sizeof(ForkFrame)));
        if (!saved) {
            vmm::destroy_address_space(child_cr3);
            return -err::ENOMEM;
        }
        saved->rbx    = f->rbx;
        saved->rbp    = f->rbp;
        saved->r12    = f->r12;
        saved->r13    = f->r13;
        saved->r14    = f->r14;
        saved->r15    = f->r15;
        saved->rip    = f->user_rip;
        saved->rflags = f->user_rflags;
        saved->rsp    = f->user_rsp;

        task::Task* child = sched::spawn("fork", fork_child_entry, saved, 16 * 1024, child_cr3);
        if (!child) {
            heap::kfree(saved);
            vmm::destroy_address_space(child_cr3);
            return -err::ENOMEM;
        }

        // shallow copy - shared `vfs::File*` pointers, no refcounting (see
        // task/task.hpp's `fds[]` comment and this project's existing
        // "leaks are an accepted gap, corruption isn't" stance elsewhere).
        for (u32 i = 0; i < task::Task::MAX_FDS; ++i) child->fds[i] = parent->fds[i];
        child->brk_start = parent->brk_start;
        child->brk_cur   = parent->brk_cur;
        child->mmap_next = parent->mmap_next;
        child->fs_base   = parent->fs_base;
        child->parent    = parent;

        return static_cast<i64>(child->id);
    }

}
