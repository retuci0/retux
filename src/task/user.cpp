#include "task/user.hpp"
#include "task/sched.hpp"
#include "task/elf.hpp"

#include "mem/pmm.hpp"
#include "mem/vmm.hpp"
#include "mem/heap.hpp"

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

    // a real Linux-ABI binary needs more stack than the hand-built test
    // blob: argv/envp/auxv strings and pointer arrays all live at the top
    // of it, above wherever the program's own call stack ends up. 32 KiB
    // total, topping out at the same address the single-page test stack
    // used to (USER_STACK_VIRT + 4096) so the two never overlap.
    constexpr u64 LINUX_STACK_TOP   = USER_STACK_VIRT + 0x1000ULL;
    constexpr u64 LINUX_STACK_PAGES = 8;
    constexpr u64 LINUX_STACK_BASE  = LINUX_STACK_TOP - LINUX_STACK_PAGES * 0x1000ULL;

    // fixed base for the anonymous-mmap bump region (task/task.hpp's
    // `mmap_next`) - a Linux-ABI process's `brk` region grows up from just
    // past its highest PT_LOAD segment (typically well under 1 MiB), so
    // picking something up here well clear of both that and the stack
    // keeps the three regions from ever colliding.
    constexpr u64 LINUX_MMAP_BASE = 0x0000'6000'0000'0000ULL;

    // ELF auxiliary vector types (Linux `<elf.h>` / `<asm/auxvec.h>`) - just
    // the ones retux's Linux-ABI stack setup actually populates.
    constexpr u64 AT_NULL   = 0;
    constexpr u64 AT_PHDR   = 3;
    constexpr u64 AT_PHENT  = 4;
    constexpr u64 AT_PHNUM  = 5;
    constexpr u64 AT_PAGESZ = 6;
    constexpr u64 AT_BASE   = 7;
    constexpr u64 AT_FLAGS  = 8;
    constexpr u64 AT_ENTRY  = 9;
    constexpr u64 AT_UID    = 11;
    constexpr u64 AT_EUID   = 12;
    constexpr u64 AT_GID    = 13;
    constexpr u64 AT_EGID   = 14;
    constexpr u64 AT_HWCAP  = 16;
    constexpr u64 AT_CLKTCK = 17;
    constexpr u64 AT_SECURE = 23;
    constexpr u64 AT_RANDOM = 25;
    constexpr u64 AT_EXECFN = 31;

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
    // returns a kernel-visible pointer to the frame (via the physmap - see
    // vmm.hpp) so the caller can write into it before ring 3 ever sees the
    // mapping.
    u8* map_new_user_page(u64 virt, u64 flags) {
        u64 phys = pmm::alloc_frame();
        vmm::map(virt, phys, flags);
        return reinterpret_cast<u8*>(vmm::phys_to_virt(phys));
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

    // drop the calling task into ring 3 at `entry_rip` with `user_rsp` as
    // its initial stack pointer. we build the frame in registers rather
    // than on the stack because the "correct" stack layout is finicky and
    // it's cleaner to just swap RSP over to a local once IRETQ's field
    // order is nailed down. never returns - once IRETQ executes, this
    // task's only path back into kernel code is via SYSCALL, which lands
    // in `syscall_entry` on the task's kernel stack top (which we're NOT
    // on anymore - we just clobbered rsp), not here.
    [[noreturn]] void enter_ring3(u64 entry_rip, u64 user_rsp) {
        IretFrame f = {
            /* rip    */ entry_rip,
            /* cs     */ USER_CODE_SELECTOR,
            /* rflags */ 0x202,  // IF=1 (bit 9), reserved bit 1 always set
            /* rsp    */ user_rsp,
            /* ss     */ USER_DATA_SELECTOR,
        };

        asm volatile(
            "mov %0, %%rsp\n"  // swap our own RSP over to point at the frame
            "iretq\n"
            :
            : "r"(&f)
            : "memory"
        );

        // unreachable, but the compiler doesn't know IRETQ never returns -
        // keep something after the asm to avoid falling off the function.
        __builtin_unreachable();
    }

    // this runs as the entry function of a normal `sched::spawn`'d kernel
    // task. it's what task creation's `task_trampoline` calls into.
    void user_task_entry(void*) {
        // copy the payload into a fresh USER-mapped code page.
        u8* code_kernel = map_new_user_page(USER_CODE_VIRT, USER_RX);
        u64 blob_size = static_cast<u64>(user_test_end - user_test_start);
        string::memcpy(code_kernel, user_test_start, blob_size);

        // give it a one-page stack too. USER_STACK_VIRT is the base of
        // the mapped page; the initial RSP goes at the top (grows down).
        map_new_user_page(USER_STACK_VIRT, USER_RW);
        u64 user_rsp_initial = USER_STACK_VIRT + 4096;

        serial::print("user: iretq to ring 3\n");
        enter_ring3(USER_CODE_VIRT, user_rsp_initial);
    }

    // fills `out[0..15]` with bytes good enough for AT_RANDOM (stack
    // protector seed, mainly) - not cryptographically secure, just needs to
    // not be the same 16 bytes every boot. xorshift64 seeded from RDTSC.
    void fill_random(u8* out) {
        u32 lo, hi;
        asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
        u64 x = (static_cast<u64>(hi) << 32) | lo;
        if (x == 0) x = 0x2545F4914F6CDD1DULL;  // xorshift can't start at 0
        for (int i = 0; i < 16; ++i) {
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
            out[i] = static_cast<u8>(x);
        }
    }

    // copies `len` bytes from a kernel buffer to a USER virtual address
    // range that's already mapped (by `linux_task_entry`, page by page) -
    // handles the range spanning multiple, physically non-contiguous pages
    // by translating each page it touches through `vmm::virt_to_phys()`,
    // same pattern `elf.cpp`'s `load_segment` uses for PT_LOAD data.
    void copy_to_user(u64 user_addr, const u8* src, u64 len) {
        u64 remaining = len;
        u64 va = user_addr;
        while (remaining > 0) {
            u64 page_base = va & ~0xFFFull;
            u64 page_off  = va - page_base;
            u64 chunk = (0x1000 - page_off < remaining) ? (0x1000 - page_off) : remaining;
            u8* kptr = reinterpret_cast<u8*>(vmm::phys_to_virt(vmm::virt_to_phys(page_base))) + page_off;
            string::memcpy(kptr, src, chunk);
            va += chunk; src += chunk; remaining -= chunk;
        }
    }

    // `arg` for `linux_task_entry` - just the path, since `elf::load()` now
    // runs INSIDE the task's own entry function rather than the caller's.
    // heap-allocated by `spawn_from_elf()` since `task::EntryFn` only
    // carries one word of payload; freed by `linux_task_entry` itself once
    // it's done reading from it.
    struct LinuxSpawnArgs {
        char path[128];
    };

    // entry function for a real Linux-ABI ELF task: loads the ELF, maps a
    // multi-page user stack, builds the `[argc][argv][envp][auxv]` block
    // Linux binaries expect to find at the initial RSP (see the "ELF loader
    // + Linux initial stack layout" plan - musl's `_start` reads this
    // straight off the stack, no argc/argv in registers), sets up this
    // task's `brk` and `mmap` regions, then drops into the entry point.
    //
    // deliberately does the ELF loading itself, rather than `spawn_from_elf`
    // doing it before `sched::spawn()`: every `vmm::map()` call here needs
    // to land in THIS task's own private address space
    // (`vmm::create_address_space()`), which is only the currently-active
    // one once the scheduler has actually switched into this task - not
    // while `spawn_from_elf()` was still running in the caller's context.
    void linux_task_entry(void* arg_ptr) {
        auto* args = static_cast<LinuxSpawnArgs*>(arg_ptr);
        char path[128];
        string::strncpy(path, args->path, sizeof(path));
        path[sizeof(path) - 1] = '\0';
        heap::kfree(args);

        elf::LoadResult result;
        if (!elf::load(path, &result)) {
            serial::print("user: couldn't load "); serial::print(path); serial::print("\n");
            return;  // task_trampoline treats a returning entry fn as exit
        }

        for (u64 i = 0; i < LINUX_STACK_PAGES; ++i) {
            map_new_user_page(LINUX_STACK_BASE + i * 0x1000ULL, USER_RW);
        }

        task::Task* self = sched::current_task();
        self->brk_start = self->brk_cur = result.highest_addr;
        self->mmap_next = LINUX_MMAP_BASE;

        // --- build the [argc][argv][envp][auxv] block ---
        // two passes: first lay out strings (their final USER addresses
        // depend only on LINUX_STACK_TOP and the total block size, which
        // isn't known until we've measured everything), then the pointer
        // arrays, which reference those addresses.
        u8 scratch[512];
        u64 off = 0;

        u64 random_off = off;
        fill_random(scratch + off);
        off += 16;

        u64 path_off = off;
        size_t path_len = string::strlen(path) + 1;
        string::memcpy(scratch + off, path, path_len);
        off += path_len;

        // 16-byte align before the pointer arrays start - not just 8. the
        // array section's own size is a multiple of 16 (see arr_size
        // below), so aligning arr_off to 16 here guarantees `total` (and
        // therefore `base` and the final rsp = base + arr_off, both
        // TOP-anchored multiples of 16 minus a multiple of 16) all land
        // 16-byte aligned too - which the SysV ABI requires of rsp at
        // process entry (rsp points at argc). aligning to 8 instead of 16
        // would make that depend on `path_len`'s parity and only work by
        // accident half the time.
        off = (off + 15) & ~15ULL;
        u64 arr_off = off;

        constexpr u64 argc = 1;    // argv[0] = path, nothing else for now
        constexpr u64 envc = 0;    // no environment variables yet
        constexpr u64 auxc = 13;   // AT_* pairs written below (PHDR, PHENT,
                                   // PHNUM, PAGESZ, BASE, FLAGS, ENTRY, UID,
                                   // EUID, GID, EGID, SECURE, RANDOM) - must
                                   // match the number of auxv(...) calls
                                   // below, or the array section gets
                                   // undersized and the copy to userspace
                                   // truncates (silently dropping AT_NULL).

        // the array section's byte size is fixed by the counts above, so
        // the base address for the whole block (and therefore every
        // string's final USER address) is now computable.
        u64 arr_size = (1 + argc + 1 + envc + 1 + auxc * 2 + 2) * 8;
        u64 total    = arr_off + arr_size;
        u64 base     = LINUX_STACK_TOP - ((total + 15) & ~15ULL);

        u64 random_addr = base + random_off;
        u64 path_addr   = base + path_off;

        u64* arr = reinterpret_cast<u64*>(scratch + arr_off);
        u64 i = 0;
        arr[i++] = argc;
        arr[i++] = path_addr;   // argv[0]
        arr[i++] = 0;           // argv NULL terminator
        arr[i++] = 0;           // envp NULL terminator (envc == 0)
        auto auxv = [&](u64 type, u64 val) { arr[i++] = type; arr[i++] = val; };
        auxv(AT_PHDR,   result.phdr_vaddr);
        auxv(AT_PHENT,  result.phentsize);
        auxv(AT_PHNUM,  result.phnum);
        auxv(AT_PAGESZ, 0x1000);
        auxv(AT_BASE,   0);        // non-PIE: no interpreter/dynamic base
        auxv(AT_FLAGS,  0);
        auxv(AT_ENTRY,  result.entry);
        auxv(AT_UID,    0); auxv(AT_EUID, 0); auxv(AT_GID, 0); auxv(AT_EGID, 0);
        auxv(AT_SECURE, 0);
        auxv(AT_RANDOM, random_addr);
        arr[i++] = AT_NULL; arr[i++] = 0;

        copy_to_user(base, scratch, total);

        u64 user_rsp_initial = base + arr_off;

        serial::print("user: iretq to Linux ELF entry\n");
        enter_ring3(result.entry, user_rsp_initial);
    }

}

namespace task::user {

    void spawn_test() {
        sched::spawn("user_test", user_task_entry, nullptr);
    }

    Task* spawn_from_elf(const char* path) {
        LinuxSpawnArgs* args = static_cast<LinuxSpawnArgs*>(heap::kmalloc(sizeof(LinuxSpawnArgs)));
        if (!args) return nullptr;
        string::strncpy(args->path, path, sizeof(args->path));
        args->path[sizeof(args->path) - 1] = '\0';

        // own private address space, attached atomically at creation (see
        // task::create()'s doc comment for why not "after" spawn()) -
        // elf::load() itself now happens inside linux_task_entry, once
        // this is the active CR3.
        u64 cr3 = vmm::create_address_space();
        return sched::spawn(path, linux_task_entry, args, 16 * 1024, cr3);
    }

}
