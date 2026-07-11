#include "task/task.hpp"

#include "cpu/fpu.hpp"

#include "mem/heap.hpp"

#include "lib/types.hpp"
#include "lib/string.hpp"


// defined in `sched.cpp` - every fresh task's manufactured stack frame
// "returns" into this instead of into a real caller.
extern "C" void task_trampoline();

namespace {

    u64 next_id = 1;

    // mirrors what switch_to() (task/switch.asm) leaves on the stack: r15..rbp
    // + rflags (low to high), then the return address it rets into. planting
    // one on a fresh stack makes the first switch_to() pop it like a real frame
    // and ret into task_trampoline.
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

        // 512-byte FXSAVE area, 16-aligned (kmalloc only gives 8) - over-
        // allocate by 15 and round up.
        u8* fpu_raw = static_cast<u8*>(heap::kmalloc(512 + 15));
        if (!fpu_raw) {
            heap::kfree(stack);
            heap::kfree(t);
            return nullptr;
        }
        u8* fpu_state = reinterpret_cast<u8*>((reinterpret_cast<u64>(fpu_raw) + 15) & ~15ULL);
        string::memcpy(fpu_state, fpu::default_state(), 512);

        // align the top down to 16 before planting the frame.
        u64 stack_top = reinterpret_cast<u64>(stack + stack_size) & ~0xFULL;

        // 8 bytes of padding above the frame so that after switch_to() pops
        // everything, RSP sits at stack_top - 8 - 16-aligned minus a qword,
        // exactly what a real `call` into task_trampoline would leave.
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
        t->fpu_raw    = fpu_raw;
        t->fpu_state  = fpu_state;
        t->parent     = nullptr;  // sys_fork() overwrites this after create() returns
        t->exit_code  = 0;
        for (u32 i = 0; i < Task::MAX_FDS; ++i) t->fds[i] = nullptr;

        size_t i = 0;
        for (; name && name[i] != '\0' && i + 1 < sizeof(t->name); ++i) {
            t->name[i] = name[i];
        }
        t->name[i] = '\0';

        return t;
    }

}
