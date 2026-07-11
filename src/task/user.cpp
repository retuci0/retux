#include "task/user.hpp"
#include "task/sched.hpp"
#include "task/elf.hpp"

#include "mem/pmm.hpp"
#include "mem/vmm.hpp"
#include "mem/heap.hpp"

#include "lib/string.hpp"
#include "lib/types.hpp"
#include "lib/errno.hpp"

#include "io/serial.hpp"


// user_test.asm's ring-3 payload, sitting as raw bytes in kernel .rodata;
// copied into a mapped USER page before dropping to ring 3.
extern "C" u8 user_test_start[];
extern "C" u8 user_test_end[];

namespace {

    // conventional user-half addresses, clear of anything else.
    constexpr u64 USER_CODE_VIRT  = 0x0000'0000'0040'0000ULL;  // 4 MiB
    constexpr u64 USER_STACK_VIRT = 0x0000'7FFF'FFFF'E000ULL;  // one page below the top

    // Linux-ABI stack: argv/envp/auxv arrays and strings live at its top,
    // above the program's own call stack. mapped eagerly (no demand-paged
    // growth), so size it for real programs - fastfetch overflows 32 KiB.
    // 512 KiB, topping out at USER_STACK_VIRT + 4096.
    constexpr u64 LINUX_STACK_TOP   = USER_STACK_VIRT + 0x1000ULL;
    constexpr u64 LINUX_STACK_PAGES = 128;
    constexpr u64 LINUX_STACK_BASE  = LINUX_STACK_TOP - LINUX_STACK_PAGES * 0x1000ULL;

    // base for the anonymous-mmap bump region - well clear of the ELF/brk
    // (low) and the stack (high) so the three never collide.
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

    // caps on argv/envp so the offset arrays can be fixed-size stack locals.
    constexpr u64 MAX_ARGC = 64;
    constexpr u64 MAX_ENVC = 32;

    // must match the DPL=3 selectors in boot.asm's GDT.
    constexpr u64 USER_CODE_SELECTOR = 0x20 | 3;
    constexpr u64 USER_DATA_SELECTOR = 0x18 | 3;

    // W^X page flags: code R-X, stack RW-, both USER.
    constexpr u64 USER_RX = vmm::PRESENT | vmm::USER;
    constexpr u64 USER_RW = vmm::PRESENT | vmm::USER | vmm::WRITABLE | vmm::NO_EXECUTE;

    // map one fresh page at `virt`, returning a kernel pointer to it (via the
    // physmap) so the caller can populate it before ring 3 sees the mapping.
    u8* map_new_user_page(u64 virt, u64 flags) {
        u64 phys = pmm::alloc_frame();
        vmm::map(virt, phys, flags);
        return reinterpret_cast<u8*>(vmm::phys_to_virt(phys));
    }

    // IRETQ pops these five in this exact order (architectural - don't reorder).
    struct IretFrame {
        u64 rip;
        u64 cs;
        u64 rflags;
        u64 rsp;
        u64 ss;
    } __attribute__((packed));

    // drop the calling task into ring 3 at entry_rip/user_rsp. never returns -
    // after IRETQ the only way back into the kernel is a SYSCALL, which lands
    // in syscall_entry, not here.
    [[noreturn]] void enter_ring3(u64 entry_rip, u64 user_rsp) {
        IretFrame f = {
            /* rip    */ entry_rip,
            /* cs     */ USER_CODE_SELECTOR,
            /* rflags */ 0x202,  // IF=1 (bit 9), reserved bit 1 always set
            /* rsp    */ user_rsp,
            /* ss     */ USER_DATA_SELECTOR,
        };

        asm volatile(
            "mov %0, %%rsp\n"  // point RSP at the frame, then IRETQ off it
            "iretq\n"
            :
            : "r"(&f)
            : "memory"
        );

        __builtin_unreachable();
    }

    // entry fn of a normal sched::spawn'd task (called via task_trampoline).
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

    // 16 bytes for AT_RANDOM (mostly the stack-protector seed). xorshift64
    // off RDTSC - not secure, just not the same every boot.
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

    // copy into an already-mapped USER range, translating each page through
    // virt_to_phys since consecutive pages aren't physically contiguous
    // (same pattern as elf.cpp's load_segment).
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

    // builds the [argc][argv][NULL][envp][NULL][auxv] block Linux binaries
    // expect at their initial RSP and copies it to the mapped stack. shared
    // by linux_task_entry (argc=1) and sys_execve (real argv/envp). argv/envp
    // are KERNEL pointers, already copied out of user memory by the caller;
    // argv[0] doubles as the program name (no AT_EXECFN).
    //
    // returns the initial user RSP, or 0 (treat as -E2BIG) if the block
    // doesn't fit in [LINUX_STACK_BASE, LINUX_STACK_TOP).
    u64 build_linux_stack(const elf::LoadResult& result,
                           u64 argc, const char* const* argv,
                           u64 envc, const char* const* envp) {
        // layout low->high: [array: argc, argv[], NULL, envp[], NULL, auxv]
        // then [strings: AT_RANDOM bytes, argv/envp strings]. rsp = base =
        // the start of the array (at argc). strings go at the TOP of the
        // stack: anything below the entry rsp is free space _start clobbers
        // with its first call, before it's even read argv[0].
        constexpr u64 auxc = 13;  // must match the auxv(...) calls below
        u64 arr_size         = (1 + argc + 1 + envc + 1 + auxc * 2 + 2) * 8;
        u64 arr_size_aligned = (arr_size + 15) & ~15ULL;  // keeps `base` (=rsp) 16-aligned

        size_t strings_size = 16;  // AT_RANDOM bytes
        for (u64 i = 0; i < argc; ++i) strings_size += string::strlen(argv[i]) + 1;
        for (u64 i = 0; i < envc; ++i) strings_size += string::strlen(envp[i]) + 1;

        u64 total = arr_size_aligned + strings_size;
        u8* scratch = static_cast<u8*>(heap::kmalloc(total));
        if (!scratch) return 0;

        // strings first (their offsets are needed to fill in the array
        // section below), starting right after the array section's space.
        u64 off = arr_size_aligned;
        u64 random_off = off;
        fill_random(scratch + off);
        off += 16;

        u64 argv_off[MAX_ARGC];
        for (u64 i = 0; i < argc; ++i) {
            argv_off[i] = off;
            size_t len = string::strlen(argv[i]) + 1;
            string::memcpy(scratch + off, argv[i], len);
            off += len;
        }
        u64 envp_off[MAX_ENVC];
        for (u64 i = 0; i < envc; ++i) {
            envp_off[i] = off;
            size_t len = string::strlen(envp[i]) + 1;
            string::memcpy(scratch + off, envp[i], len);
            off += len;
        }

        u64 base = LINUX_STACK_TOP - ((total + 15) & ~15ULL);

        if (base < LINUX_STACK_BASE) {
            // too big for the 32KiB stack. bail cleanly - otherwise
            // copy_to_user hits an unmapped page, virt_to_phys returns 0, and
            // phys_to_virt(0) corrupts low physical memory.
            heap::kfree(scratch);
            return 0;
        }

        u64 random_addr = base + random_off;

        u64* arr = reinterpret_cast<u64*>(scratch);  // array section starts at offset 0 now
        u64 i = 0;
        arr[i++] = argc;
        for (u64 j = 0; j < argc; ++j) arr[i++] = base + argv_off[j];
        arr[i++] = 0;  // argv NULL terminator
        for (u64 j = 0; j < envc; ++j) arr[i++] = base + envp_off[j];
        arr[i++] = 0;  // envp NULL terminator
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
        heap::kfree(scratch);

        return base;  // rsp = &argc, exactly the start of the array section
    }

    // linux_task_entry's `arg` - just the path (task::EntryFn carries one
    // word). heap-allocated by spawn_from_elf, freed by linux_task_entry.
    struct LinuxSpawnArgs {
        char path[128];
    };

    // entry fn for a Linux-ABI ELF task: loads the ELF, maps the stack, builds
    // the initial-RSP block, sets up brk/mmap, then drops into ring 3.
    //
    // does the ELF loading itself rather than in spawn_from_elf, because every
    // vmm::map() here must land in THIS task's private address space, which is
    // only the active CR3 once the scheduler has switched into this task - not
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

        // argc=1 (argv[0]=path); spawn_from_elf has no real argv/envp. real
        // ones come through sys_execve.
        const char* argv[1] = {path};
        u64 user_rsp_initial = build_linux_stack(result, 1, argv, 0, nullptr);
        if (user_rsp_initial == 0) {
            serial::print("user: argv/envp block too big for the mapped stack\n");
            return;  // task_trampoline treats a returning entry fn as exit
        }

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

        // private address space, attached at creation (see task::create's
        // comment for why not after spawn()); elf::load runs in linux_task_entry.
        u64 cr3 = vmm::create_address_space();
        return sched::spawn(path, linux_task_entry, args, 16 * 1024, cr3);
    }

    // execve(path, argv, envp) - replaces the calling task's image in place
    // (same pid/Task*/kernel stack), unlike spawn_from_elf. path/argv/envp are
    // USER pointers into the current (about-to-be-replaced) address space, so
    // copy every string into the kernel FIRST, before any teardown.
    //
    // stages the whole new image (address space, ELF, stack, arg block) before
    // destroying the old one, so any failure can switch back to the intact old
    // space and return -errno instead of killing the caller.
    i64 sys_execve(const char* path_user, char* const* argv_user, char* const* envp_user) {
        task::Task* self = sched::current_task();

        char path[128];
        string::strncpy(path, path_user, sizeof(path));
        path[sizeof(path) - 1] = '\0';

        u64 argc = 0;
        char* argv_k[MAX_ARGC];
        if (argv_user) {
            while (argv_user[argc] && argc < MAX_ARGC) {
                size_t len = string::strlen(argv_user[argc]) + 1;
                char* copy = static_cast<char*>(heap::kmalloc(len));
                string::memcpy(copy, argv_user[argc], len);
                argv_k[argc] = copy;
                ++argc;
            }
        }
        u64 envc = 0;
        char* envp_k[MAX_ENVC];
        if (envp_user) {
            while (envp_user[envc] && envc < MAX_ENVC) {
                size_t len = string::strlen(envp_user[envc]) + 1;
                char* copy = static_cast<char*>(heap::kmalloc(len));
                string::memcpy(copy, envp_user[envc], len);
                envp_k[envc] = copy;
                ++envc;
            }
        }

        auto free_copies = [&]() {
            for (u64 i = 0; i < argc; ++i) heap::kfree(argv_k[i]);
            for (u64 i = 0; i < envc; ++i) heap::kfree(envp_k[i]);
        };

        // build the new address space and switch in. update the field BEFORE
        // the mov cr3: if a timer preempts between them, schedule()'s switch-in
        // reloads CR3 from this same field, so both agree either way.
        u64 old_cr3 = self->cr3;
        u64 new_cr3 = vmm::create_address_space();
        self->cr3 = new_cr3;
        asm volatile("mov %0, %%cr3" : : "r"(new_cr3) : "memory");

        elf::LoadResult result;
        if (!elf::load(path, &result)) {
            self->cr3 = old_cr3;
            asm volatile("mov %0, %%cr3" : : "r"(old_cr3) : "memory");
            vmm::destroy_address_space(new_cr3);  // nothing usable was ever mapped in it
            free_copies();
            return -err::ENOENT;
        }

        for (u64 i = 0; i < LINUX_STACK_PAGES; ++i) {
            map_new_user_page(LINUX_STACK_BASE + i * 0x1000ULL, USER_RW);
        }
        self->brk_start = self->brk_cur = result.highest_addr;
        self->mmap_next = LINUX_MMAP_BASE;

        u64 user_rsp = build_linux_stack(
            result, argc, const_cast<const char* const*>(argv_k),
            envc, const_cast<const char* const*>(envp_k));
        free_copies();

        if (user_rsp == 0) {
            self->cr3 = old_cr3;
            asm volatile("mov %0, %%cr3" : : "r"(old_cr3) : "memory");
            vmm::destroy_address_space(new_cr3);
            return -err::E2BIG;
        }

        // committed - free the old space (no longer active, and everything we
        // needed from it was already copied out).
        if (old_cr3 != 0) vmm::destroy_address_space(old_cr3);

        // doesn't return through the syscall tail - matching manual swapgs
        // first, same GS_BASE reason as sys_exit.
        asm volatile("swapgs" ::: "memory");
        enter_ring3(result.entry, user_rsp);
    }

}
