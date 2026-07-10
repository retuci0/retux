#include "task/task.hpp"

#include "mem/heap.hpp"

#include "lib/types.hpp"


// defined in `sched.cpp` - every fresh task's manufactured stack frame
// "returns" into this instead of into a real caller.
extern "C" void task_trampoline();

namespace {

    u64 next_id = 1;

    // mirrors exactly what `switch_to()` (task/switch.asm) leaves on the
    // stack: it pushes rflags, rbp, rbx, r12, r13, r14, r15 in that order
    // (so r15 ends up closest to the top - lowest address - since each
    // push moves further down), then later pops them in the reverse
    // order, and finally `ret`s into whatever return address sits above
    // all of that. this struct's field order matches low address ->
    // high address exactly, so planting one directly on a fresh task's
    // otherwise-empty stack makes `switch_to()` pop it exactly like it
    // would pop a real one, then `ret` straight into `task_trampoline`.
    struct InitialFrame {
        u64 r15, r14, r13, r12, rbx, rbp;
        u64 rflags;
        u64 return_addr;
    };

}

namespace task {

    Task* create(const char* name, EntryFn entry, void* arg, u64 stack_size, u64 cr3) {
        Task* t = static_cast<Task*>(heap::kmalloc(sizeof(Task)));
        if (!t) return nullptr;

        u8* stack = static_cast<u8*>(heap::kmalloc(stack_size));
        if (!stack) {
            heap::kfree(t);
            return nullptr;
        }

        // `kmalloc` only guarantees 8-byte alignment - align the top down
        // to 16 bytes ourselves before planting the initial frame.
        u64 stack_top = reinterpret_cast<u64>(stack + stack_size) & ~0xFULL;

        // leave 8 bytes of padding above the frame so that once
        // `switch_to()` pops everything off (including `return_addr`),
        // RSP sits at `stack_top - 8` - i.e. 16-aligned minus one qword,
        // exactly what a real `call` instruction would leave behind for
        // `task_trampoline` to start executing with.
        u64 frame_addr = stack_top - sizeof(InitialFrame) - 8;
        InitialFrame* frame = reinterpret_cast<InitialFrame*>(frame_addr);

        frame->r15 = 0;
        frame->r14 = 0;
        frame->r13 = 0;
        frame->r12 = 0;
        frame->rbx = 0;
        frame->rbp = 0;
        frame->rflags     = 0x202;  // IF set (bit 9), reserved bit 1 always set
        frame->return_addr = reinterpret_cast<u64>(&task_trampoline);

        t->rsp              = frame_addr;
        t->stack_base       = reinterpret_cast<u64>(stack);
        t->stack_size       = stack_size;
        t->kernel_stack_top = stack_top;  // the aligned top computed above
        t->id               = next_id++;
        t->state      = State::Ready;
        t->entry      = entry;
        t->arg        = arg;
        t->next       = nullptr;

        t->fs_base    = 0;
        t->cr3        = cr3;
        t->brk_start  = 0;
        t->brk_cur    = 0;
        t->mmap_next  = 0;
        for (u32 i = 0; i < Task::MAX_FDS; ++i) t->fds[i] = nullptr;

        size_t i = 0;
        for (; name && name[i] != '\0' && i + 1 < sizeof(t->name); ++i) {
            t->name[i] = name[i];
        }
        t->name[i] = '\0';

        return t;
    }

}
