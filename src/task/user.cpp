#include "task/user.hpp"
#include "task/sched.hpp"

#include "mem/pmm.hpp"
#include "mem/vmm.hpp"

#include "lib/string.hpp"
#include "lib/types.hpp"

#include "io/serial.hpp"


// symbols exported by `task/user_test.asm` - the ring-3 payload sits as
// raw bytes in the kernel's `.rodata`, and we copy it into a mapped
// USER page before dropping to ring 3.
extern "C" u8 user_test_start[];
extern "C" u8 user_test_end[];

namespace {

    // arbitrary but conventional - low enough to be well inside the user
    // half of the canonical address space (0x0000_0000_0000_0000 up to
    // 0x0000_7FFF_FFFF_FFFF), high enough that nothing else has claimed it.
    constexpr u64 USER_CODE_VIRT  = 0x0000'0000'0040'0000ULL;  // 4 MiB
    constexpr u64 USER_STACK_VIRT = 0x0000'7FFF'FFFF'E000ULL;  // one page below the top

    // must match the DPL=3 selectors in boot.asm's GDT.
    constexpr u64 USER_CODE_SELECTOR = 0x20 | 3;
    constexpr u64 USER_DATA_SELECTOR = 0x18 | 3;

    // per-mapping flag combos. USER on all of them (obviously); code stays
    // executable-but-not-writable, stack stays writable-but-not-executable
    // - even before the ELF loader gets to do proper per-segment W^X, at
    // least these hand-mapped pages set the right example.
    constexpr u64 USER_RX = vmm::PRESENT | vmm::USER;
    constexpr u64 USER_RW = vmm::PRESENT | vmm::USER | vmm::WRITABLE | vmm::NO_EXECUTE;

    // maps one 4KB page at `virt`, backed by a fresh PMM frame, with `flags`.
    // returns the (identity-mapped) kernel-visible pointer to the frame so
    // the caller can write into it before ring 3 ever sees the mapping.
    u8* map_new_user_page(u64 virt, u64 flags) {
        u64 phys = pmm::alloc_frame();
        vmm::map(virt, phys, flags);
        // the first 4 GiB are identity-mapped by `remap_kernel()`'s starting
        // state (see `boot.asm`), so the physical address is directly
        // usable as a kernel pointer.
        return reinterpret_cast<u8*>(phys);
    }

    // hand-crafted `iretq` frame - IRETQ pops these five off the stack in
    // this exact order and jumps to userspace. field order is defined by
    // the x86-64 architecture, not us; don't reorder.
    struct IretFrame {
        u64 rip;
        u64 cs;
        u64 rflags;
        u64 rsp;
        u64 ss;
    } __attribute__((packed));

    // this runs as the entry function of a normal `sched::spawn`'d kernel
    // task. it's what task creation's `task_trampoline` calls into. once
    // we hit `iretq`, the CPU jumps to ring 3 and the entry function
    // "never returns" as far as C++ is concerned - the only way back is
    // via SYSCALL, at which point we're on this same kernel stack again
    // but inside `syscall_dispatch()`, not here.
    void user_task_entry(void*) {
        // copy the payload into a fresh USER-mapped code page.
        u8* code_kernel = map_new_user_page(USER_CODE_VIRT, USER_RX);
        u64 blob_size = static_cast<u64>(user_test_end - user_test_start);
        string::memcpy(code_kernel, user_test_start, blob_size);

        // give it a one-page stack too. USER_STACK_VIRT is the base of
        // the mapped page; the initial RSP goes at the top (grows down).
        map_new_user_page(USER_STACK_VIRT, USER_RW);
        u64 user_rsp_initial = USER_STACK_VIRT + 4096;

        // drop to ring 3 via IRETQ with a hand-crafted frame. we build
        // it in registers rather than on the stack because the "correct"
        // stack layout is finicky and it's cleaner to just `push` in the
        // right order in inline asm.
        IretFrame f = {
            /* rip    */ USER_CODE_VIRT,
            /* cs     */ USER_CODE_SELECTOR,
            /* rflags */ 0x202,  // IF=1 (bit 9), reserved bit 1 always set
            /* rsp    */ user_rsp_initial,
            /* ss     */ USER_DATA_SELECTOR,
        };

        serial::print("user: iretq to ring 3\n");

        asm volatile(
            "mov %0, %%rsp\n"  // swap our own RSP over to point at the frame
            "iretq\n"
            :
            : "r"(&f)
            : "memory"
        );

        // unreachable - once IRETQ executes, this task's only path back into
        // kernel code is via SYSCALL, which lands in `syscall_entry` on the
        // task's kernel stack top (which we're NOT on anymore - we just
        // clobbered rsp), not here. the compiler doesn't know that though,
        // so keep something after the asm to avoid falling off the function.
        __builtin_unreachable();
    }

}

namespace task::user {

    void spawn_test() {
        sched::spawn("user_test", user_task_entry, nullptr);
    }

}
