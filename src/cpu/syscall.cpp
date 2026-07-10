#include "cpu/syscall.hpp"

#include "cpu/cpu.hpp"

#include "task/sched.hpp"

#include "io/hpet.hpp"
#include "io/pit.hpp"

#include "mem/pmm.hpp"
#include "mem/vmm.hpp"
#include "mem/heap.hpp"

#include "fs/vfs.hpp"

#include "tty/tty.hpp"

#include "io/serial.hpp"

#include "lib/types.hpp"
#include "lib/errno.hpp"
#include "lib/string.hpp"


// low-level entry point defined in `cpu/syscall_entry.asm`. userspace
// executes `syscall`, the CPU jumps here, and eventually this stub sysrets
// back with `rax` = the value returned by `syscall_dispatch()` below.
extern "C" void syscall_entry();

namespace {

    constexpr u32 IA32_EFER  = 0xC000'0080;
    constexpr u32 IA32_STAR  = 0xC000'0081;
    constexpr u32 IA32_LSTAR = 0xC000'0082;
    constexpr u32 IA32_FMASK = 0xC000'0084;

    constexpr u64 EFER_SCE = 1ULL << 0;  // System Call Extensions

    // must match the GDT layout in boot.asm:
    //   STAR[47:32] -> kernel CS (kernel SS derived by CPU as +8)
    //   STAR[63:48] -> base for user CS/SS derived by SYSRET as +16 / +8
    //                  so with 0x10 here, SYSRET picks user_data at 0x18
    //                  and user_code at 0x20, both DPL=3.
    constexpr u16 SYSCALL_KERNEL_CS_BASE = 0x08;
    constexpr u16 SYSRET_USER_SEG_BASE   = 0x10;

    // --- syscall implementations ---

    // write(fd, buf, len). fd 1 -> tty, fd 2 -> serial. returns byte count
    // on success, -1 on unknown fd. no validation of `buf` yet: this is
    // called from ring 3 but the user pointer is trusted for now, since
    // Landing A's only user program is a static kernel-embedded blob whose
    // strings we ourselves mapped. once the ELF loader lands, this grows a
    // real "does virt fall in the caller's user VMA?" check.
    i64 sys_write(u64 fd, const char* buf, u64 len) {
        if (fd != 1 && fd != 2) return -1;
        for (u64 i = 0; i < len; ++i) {
            if (fd == 1) tty::print(buf[i]);
            else         serial::print(buf[i]);
        }
        return static_cast<i64>(len);
    }

    [[noreturn]] void sys_exit(u64 code) {
        (void) code;  // not surfaced anywhere yet

        // `syscall_entry`'s tail (cpu/syscall_entry.asm) does a matching
        // `swapgs` right before `sysret`, to swap GS_BASE back from
        // `&g_cpu_local` (which the ENTRY swapgs put there) to whatever the
        // caller's GS_BASE was. `exit_current()` below never returns
        // through that tail - it jumps straight into some other task via
        // `switch_to()` - so without this, GS_BASE stays stuck at
        // `&g_cpu_local` forever. the NEXT task to enter ring 3 for the
        // first time (via a raw `iretq`, not `sysret` - see
        // `task/user.cpp`'s `enter_ring3`) inherits that leftover kernel
        // GS_BASE, and ITS first syscall's entry `swapgs` then toggles GS
        // to a completely wrong state (swapping the stale kernel value
        // into IA32_KernelGSBase instead of restoring it) - `[gs:...]`
        // ends up resolving against address 0 instead of `g_cpu_local`,
        // reading garbage. doing the matching swapgs here, manually,
        // before abandoning the normal return path, keeps the pairing
        // balanced regardless of which task runs next.
        // 
        // TL;DR: `exit_current()` skips the normal syscall return, leaving GS stuck to the kernel value. 
        // that breaks the next task's first syscall (reads garbage from address 0). 
        // fix: manually swapgs back before the jump to keep the pairing balanced.
        asm volatile("swapgs" ::: "memory");

        sched::exit_current();  // marks Dead, switches away, never returns
    }

    // writev(fd, iov, iovcnt) - musl's stdio flush path uses this instead
    // of plain write() once buffering kicks in. same fd/trust rules as
    // sys_write above, just looped over each chunk.
    struct iovec { void* iov_base; u64 iov_len; };

    i64 sys_writev(u64 fd, const iovec* iov, u64 iovcnt) {
        i64 total = 0;
        for (u64 i = 0; i < iovcnt; ++i) {
            i64 n = sys_write(fd, static_cast<const char*>(iov[i].iov_base), iov[i].iov_len);
            if (n < 0) return (total > 0) ? total : n;
            total += n;
        }
        return total;
    }

    // page-granular USER mapping flags, shared by brk/mmap growth below.
    // matches `task/user.cpp`'s USER_RW - duplicated rather than shared
    // since that one's `static` to an anonymous namespace in another TU.
    constexpr u64 USER_RW = vmm::PRESENT | vmm::USER | vmm::WRITABLE | vmm::NO_EXECUTE;

    constexpr u64 PAGE_SIZE = 0x1000;
    inline u64 page_down(u64 x) { return x & ~(PAGE_SIZE - 1); }
    inline u64 page_up(u64 x)   { return (x + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1); }

    // brk(addr) - Linux semantics: NOT a normal "-errno on failure" syscall.
    // it always returns the resulting break value - unchanged from before
    // if the request was invalid/OOM, the new one if it succeeded. glibc/
    // musl's brk() wrapper compares the return against what it asked for
    // to decide whether it "failed", the raw syscall never returns negative.
    u64 sys_brk(u64 addr) {
        task::Task* self = sched::current_task();
        if (addr == 0 || addr < self->brk_start) return self->brk_cur;

        u64 old_top = page_up(self->brk_cur);
        u64 new_top = page_up(addr);

        if (new_top > old_top) {
            for (u64 va = old_top; va < new_top; va += PAGE_SIZE) {
                u64 phys = pmm::alloc_frame();
                vmm::map(va, phys, USER_RW);
                string::memset(reinterpret_cast<void*>(vmm::phys_to_virt(phys)), 0, PAGE_SIZE);
            }
        } else if (new_top < old_top) {
            for (u64 va = new_top; va < old_top; va += PAGE_SIZE) {
                u64 phys = vmm::virt_to_phys(va);
                vmm::unmap(va);
                if (phys) pmm::free_frame(phys);
            }
        }

        self->brk_cur = addr;
        return self->brk_cur;
    }

    constexpr u64 PROT_WRITE     = 0x2;
    constexpr u64 PROT_EXEC      = 0x4;
    constexpr u64 MAP_ANONYMOUS  = 0x20;

    // mmap(addr, len, prot, flags, fd, off) - anonymous-only for now (see
    // the plan doc's Phase 4): a real Linux binary's malloc (musl's
    // mallocng) leans on this for its arena, not just brk. bump-allocates
    // from `Task::mmap_next` - `munmap` below frees the physical frames but
    // doesn't reclaim the VA range, an intentional simplification (same
    // spirit as `elf.cpp`'s "correctness first" - nothing here reuses
    // address ranges, so no risk of a stale mapping aliasing a new one).
    // returns a raw address on success or -errno on failure, exactly like
    // any other syscall - musl/glibc's `mmap()` wrapper is the one that
    // turns "return value in [-4095,-1]" into `MAP_FAILED` + `errno`.
    i64 sys_mmap(u64 addr, u64 len, u64 prot, u64 flags, u64 fd, u64 off) {
        (void)addr; (void)fd; (void)off;
        if (!(flags & MAP_ANONYMOUS)) return -err::ENODEV;  // no file-backed mmap yet
        if (len == 0) return -err::EINVAL;

        task::Task* self = sched::current_task();
        u64 map_len = page_up(len);
        u64 base = self->mmap_next;
        self->mmap_next = base + map_len;

        u64 pflags = vmm::PRESENT | vmm::USER;
        if (prot & PROT_WRITE)      pflags |= vmm::WRITABLE;
        if (!(prot & PROT_EXEC))    pflags |= vmm::NO_EXECUTE;

        for (u64 va = base; va < base + map_len; va += PAGE_SIZE) {
            u64 phys = pmm::alloc_frame();
            vmm::map(va, phys, pflags);
            string::memset(reinterpret_cast<void*>(vmm::phys_to_virt(phys)), 0, PAGE_SIZE);
        }
        return static_cast<i64>(base);
    }

    i64 sys_munmap(u64 addr, u64 len) {
        u64 start = page_down(addr);
        u64 end   = page_up(addr + len);
        for (u64 va = start; va < end; va += PAGE_SIZE) {
            u64 phys = vmm::virt_to_phys(va);
            vmm::unmap(va);
            if (phys) pmm::free_frame(phys);
        }
        return 0;
    }

    i64 sys_mprotect(u64 addr, u64 len, u64 prot) {
        u64 start = page_down(addr);
        u64 end   = page_up(addr + len);
        u64 pflags = vmm::PRESENT | vmm::USER;
        if (prot & PROT_WRITE)   pflags |= vmm::WRITABLE;
        if (!(prot & PROT_EXEC)) pflags |= vmm::NO_EXECUTE;
        for (u64 va = start; va < end; va += PAGE_SIZE) vmm::protect(va, pflags);
        return 0;
    }

    // ---- file I/O ----
    // fd 0/1/2 are special-cased (no real vfs::File backs them); fds 3.. are
    // real vfs::File* in the current task's `fds[]` table. no cwd tracking
    // yet, so `dirfd` is ignored entirely - every path is resolved from
    // root regardless, same as `vfs::open()` already does.

    constexpr u64 STAT_SIZE = 144;  // sizeof(struct stat), x86-64 Linux ABI

    i64 alloc_fd(task::Task* self, vfs::File* f) {
        for (u32 i = 3; i < task::Task::MAX_FDS; ++i) {
            if (!self->fds[i]) { self->fds[i] = f; return static_cast<i64>(i); }
        }
        return -err::EMFILE;
    }

    // valid, in-range, currently-open real fd - null (not a fault) for
    // fd 0/1/2 or anything out of bounds/closed, so callers can tell those
    // apart from a legitimately-open file with one check.
    vfs::File* real_file(task::Task* self, u64 fd) {
        if (fd < 3 || fd >= task::Task::MAX_FDS) return nullptr;
        return self->fds[fd];
    }

    i64 sys_openat(u64 dirfd, const char* path, u64 flags, u64 mode) {
        (void)dirfd; (void)flags; (void)mode;  // write flags/modes: no writable
                                                // filesystem exists yet either
        vfs::File* f = vfs::open(path);
        if (!f) return -err::ENOENT;
        task::Task* self = sched::current_task();
        i64 fd = alloc_fd(self, f);
        if (fd < 0) heap::kfree(f);
        return fd;
    }

    i64 sys_read(u64 fd, void* buf, u64 len) {
        if (fd == 0) return 0;  // no stdin backing yet - reads as EOF
        if (fd == 1 || fd == 2) return -err::EBADF;
        vfs::File* f = real_file(sched::current_task(), fd);
        if (!f) return -err::EBADF;
        ssize_t n = f->inode->ops->read(f->inode, f->offset, buf, len);
        if (n < 0) return -err::EIO;
        f->offset += static_cast<u64>(n);
        return n;
    }

    i64 sys_close(u64 fd) {
        task::Task* self = sched::current_task();
        vfs::File* f = real_file(self, fd);
        if (!f) return -err::EBADF;
        heap::kfree(f);
        self->fds[fd] = nullptr;
        return 0;
    }

    constexpr u64 SEEK_SET = 0, SEEK_CUR = 1, SEEK_END = 2;

    i64 sys_lseek(u64 fd, i64 offset, u64 whence) {
        vfs::File* f = real_file(sched::current_task(), fd);
        if (!f) return -err::EBADF;
        i64 base;
        switch (whence) {
            case SEEK_SET: base = 0; break;
            case SEEK_CUR: base = static_cast<i64>(f->offset); break;
            case SEEK_END: base = static_cast<i64>(f->inode->size); break;
            default: return -err::EINVAL;
        }
        i64 result = base + offset;
        if (result < 0) return -err::EINVAL;
        f->offset = static_cast<u64>(result);
        return result;
    }

    // fills the Linux x86-64 `struct stat` layout (144 bytes) - see
    // <bits/stat.h> - byte-offset writes rather than a matching struct
    // definition since it's only used here and the padding is fiddly.
    void fill_stat(u8* out, u64 size, bool is_dir) {
        string::memset(out, 0, STAT_SIZE);
        *reinterpret_cast<u64*>(out + 16) = 1;  // st_nlink
        u32 mode = (is_dir ? 0040000u : 0100000u) | 0644u;  // S_IFDIR|S_IFREG + rw-r--r--
        *reinterpret_cast<u32*>(out + 24) = mode;            // st_mode
        *reinterpret_cast<i64*>(out + 48) = static_cast<i64>(size);  // st_size
        *reinterpret_cast<i64*>(out + 56) = 512;                     // st_blksize
        *reinterpret_cast<i64*>(out + 64) = static_cast<i64>((size + 511) / 512);  // st_blocks
    }

    i64 sys_fstat(u64 fd, u8* statbuf) {
        if (fd == 0 || fd == 1 || fd == 2) {
            string::memset(statbuf, 0, STAT_SIZE);
            *reinterpret_cast<u64*>(statbuf + 16) = 1;
            *reinterpret_cast<u32*>(statbuf + 24) = 0020000u | 0620u;  // S_IFCHR
            return 0;
        }
        vfs::File* f = real_file(sched::current_task(), fd);
        if (!f) return -err::EBADF;
        fill_stat(statbuf, f->inode->size, f->inode->is_dir);
        return 0;
    }

    constexpr u64 TCGETS = 0x5401;

    i64 sys_ioctl(u64 fd, u64 request, u64 argp) {
        if ((fd == 0 || fd == 1 || fd == 2) && request == TCGETS) {
            // callers only check the return value (0 => "yes, a tty") -
            // zeroing is enough, nothing reads specific termios fields yet.
            string::memset(reinterpret_cast<void*>(argp), 0, 60);  // sizeof(struct termios)
            return 0;
        }
        return -err::ENOTTY;
    }

    // ---- identity / misc ----
    // dummy but stable answers - retux has no real users/groups/threads.
    // `getpid` reuses the task id `task::create()` already hands out.

    i64 sys_getpid() { return static_cast<i64>(sched::current_task()->id); }

    // fills one of the 6 fixed-size (UTSNAME_LENGTH=65 on Linux) fields of
    // `struct utsname`, NUL-padding the rest.
    void utsname_field(u8* dest, const char* value) {
        size_t i = 0;
        for (; value[i] != '\0' && i < 64; ++i) dest[i] = static_cast<u8>(value[i]);
        for (; i < 65; ++i) dest[i] = 0;
    }

    // uname() - deliberately claims to BE Linux (not "retux"): the goal is
    // binary compatibility, and autoconf-generated / `uname`-branching code
    // expects to see "Linux", the same trick WSL1 and BSD's Linuxulator play.
    i64 sys_uname(u8* buf) {
        utsname_field(buf + 0 * 65,   "Linux");
        utsname_field(buf + 1 * 65,   "retux");
        utsname_field(buf + 2 * 65,   "5.0.0");
        utsname_field(buf + 3 * 65,   "retux");
        utsname_field(buf + 4 * 65,   "x86_64");
        utsname_field(buf + 5 * 65,   "");
        return 0;
    }

    // no wall clock - just uptime, from whichever timer main.cpp picked
    // (hpet::init() succeeding vs falling back to pit::init(), see
    // main.cpp). guards against querying the OTHER driver's uninitialized
    // state (hpet::milliseconds() divides by a `configured_hz` that's only
    // set once `hpet::init()` actually ran, and would divide-by-zero
    // otherwise) by checking whether HPET has produced any ticks yet.
    i64 sys_clock_gettime(u64 clk_id, u8* ts) {
        (void)clk_id;
        u64 ms = (hpet::ticks() > 0) ? hpet::milliseconds() : pit::ticks();
        *reinterpret_cast<i64*>(ts + 0) = static_cast<i64>(ms / 1000);
        *reinterpret_cast<i64*>(ts + 8) = static_cast<i64>((ms % 1000) * 1'000'000);
        return 0;
    }

    // same xorshift/rdtsc source as task/user.cpp's AT_RANDOM filler - not
    // cryptographically secure, just needs to not be all-zero every boot.
    i64 sys_getrandom(u8* buf, u64 len, u64 flags) {
        (void)flags;
        u32 lo, hi;
        asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
        u64 x = (static_cast<u64>(hi) << 32) | lo;
        if (x == 0) x = 0x2545F4914F6CDD1DULL;
        for (u64 i = 0; i < len; ++i) {
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
            buf[i] = static_cast<u8>(x);
        }
        return static_cast<i64>(len);
    }

    constexpr u64 ARCH_SET_FS = 0x1002;
    constexpr u64 ARCH_GET_FS = 0x1003;

    // arch_prctl(code, addr) - musl's `_start` -> `__init_tp` calls this
    // directly (not through a wrapper) to point the FS segment at its TLS
    // block, before main() ever runs. `IA32_FS_BASE` is restored per-task
    // in `sched.cpp`'s `schedule()` from `Task::fs_base`, since more than
    // one ring-3 task could in principle be resident later.
    i64 sys_arch_prctl(u64 code, u64 addr) {
        task::Task* self = sched::current_task();
        if (code == ARCH_SET_FS) {
            cpu::wrmsr(cpu::IA32_FS_BASE, addr);
            self->fs_base = addr;
            return 0;
        }
        if (code == ARCH_GET_FS) {
            // trusted user pointer, same trust model as sys_write's `buf`.
            *reinterpret_cast<u64*>(addr) = self->fs_base;
            return 0;
        }
        return -err::EINVAL;
    }

}

// called from the SYSCALL entry stub with a pointer to the Frame it built
// on the kernel stack. return value goes into `rax` on SYSRET.
extern "C" u64 syscall_dispatch(syscall::Frame* f) {
    using namespace syscall;
    switch (f->num) {
        case SYS_WRITE:
            return static_cast<u64>(sys_write(
                f->arg0,
                reinterpret_cast<const char*>(f->arg1),
                f->arg2));
        case SYS_READ:
            return static_cast<u64>(sys_read(f->arg0, reinterpret_cast<void*>(f->arg1), f->arg2));
        case SYS_CLOSE:
            return static_cast<u64>(sys_close(f->arg0));
        case SYS_FSTAT:
            return static_cast<u64>(sys_fstat(f->arg0, reinterpret_cast<u8*>(f->arg1)));
        case SYS_LSEEK:
            return static_cast<u64>(sys_lseek(f->arg0, static_cast<i64>(f->arg1), f->arg2));
        case SYS_IOCTL:
            return static_cast<u64>(sys_ioctl(f->arg0, f->arg1, f->arg2));
        case SYS_OPENAT:
            return static_cast<u64>(sys_openat(f->arg0, reinterpret_cast<const char*>(f->arg1), f->arg2, f->arg3));
        case SYS_OPEN:
            // legacy open(path, flags, mode) == openat(AT_FDCWD, ...) -
            // dirfd is ignored by sys_openat anyway (no cwd tracking).
            return static_cast<u64>(sys_openat(0, reinterpret_cast<const char*>(f->arg0), f->arg1, f->arg2));
        case SYS_BRK:
            return sys_brk(f->arg0);
        case SYS_MMAP:
            return static_cast<u64>(sys_mmap(f->arg0, f->arg1, f->arg2, f->arg3, f->arg4, f->arg5));
        case SYS_MUNMAP:
            return static_cast<u64>(sys_munmap(f->arg0, f->arg1));
        case SYS_MPROTECT:
            return static_cast<u64>(sys_mprotect(f->arg0, f->arg1, f->arg2));
        case SYS_WRITEV:
            return static_cast<u64>(sys_writev(
                f->arg0,
                reinterpret_cast<const iovec*>(f->arg1),
                f->arg2));
        case SYS_GETPID:
            return static_cast<u64>(sys_getpid());
        case SYS_UNAME:
            return static_cast<u64>(sys_uname(reinterpret_cast<u8*>(f->arg0)));
        case SYS_GETUID:
        case SYS_GETEUID:
        case SYS_GETGID:
        case SYS_GETEGID:
            return 0;
        case SYS_CLOCK_GETTIME:
            return static_cast<u64>(sys_clock_gettime(f->arg0, reinterpret_cast<u8*>(f->arg1)));
        case SYS_GETRANDOM:
            return static_cast<u64>(sys_getrandom(reinterpret_cast<u8*>(f->arg0), f->arg1, f->arg2));
        case SYS_ARCH_PRCTL:
            return static_cast<u64>(sys_arch_prctl(f->arg0, f->arg1));
        case SYS_SET_TID_ADDRESS:
            // real thread-id tracking is meaningless single-threaded -
            // return a fake but stable tid so libc has *something*.
            return 1;
        case SYS_SET_ROBUST_LIST:
        case SYS_RT_SIGACTION:
        case SYS_RT_SIGPROCMASK:
            // best-effort no-ops: musl's single-threaded startup path treats
            // these as optional and doesn't check hard on failure, but
            // returning success (rather than -ENOSYS) skips it asking twice.
            return 0;
        case SYS_EXIT:
        case SYS_EXIT_GROUP:
            sys_exit(f->arg0);
            // unreachable - sys_exit is [[noreturn]]
        default:
            // bring-up aid: a real Linux binary hitting an unimplemented
            // syscall should tell us its number instead of just misbehaving
            // silently or getting a generic -1 - that log line is what
            // drives which syscall gets implemented next.
            serial::print("syscall: unimplemented number ");
            serial::print_dec(f->num);
            serial::print("\n");
            return static_cast<u64>(-err::ENOSYS);
    }
}

namespace syscall {

    void init() {
        // STAR: high 32 bits pick the CS/SS pairs SYSCALL / SYSRET use.
        u64 star = (static_cast<u64>(SYSRET_USER_SEG_BASE)   << 48) |
                   (static_cast<u64>(SYSCALL_KERNEL_CS_BASE) << 32);
        cpu::wrmsr(IA32_STAR, star);

        // LSTAR: the address SYSCALL jumps to in 64-bit mode.
        cpu::wrmsr(IA32_LSTAR, reinterpret_cast<u64>(&syscall_entry));

        // FMASK: bits set here get CLEARED in RFLAGS on SYSCALL entry.
        // clearing IF disables interrupts across the CS/RSP swap window
        // in the entry stub - otherwise an IRQ arriving between "swapgs"
        // and "switch to kernel stack" would run with user RSP and blow
        // straight through some untrusted address. clearing DF keeps
        // System V ABI happy (kernel string ops assume DF=0).
        constexpr u64 RFLAGS_IF = 1ULL << 9;
        constexpr u64 RFLAGS_DF = 1ULL << 10;
        cpu::wrmsr(IA32_FMASK, RFLAGS_IF | RFLAGS_DF);

        // finally, actually turn SYSCALL/SYSRET on. up till now they'd
        // #UD - EFER.SCE is off by default even in long mode.
        u64 efer = cpu::rdmsr(IA32_EFER);
        cpu::wrmsr(IA32_EFER, efer | EFER_SCE);

        serial::print("syscall: SYSCALL/SYSRET enabled\n");
    }

}
