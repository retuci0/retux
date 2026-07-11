#include "cpu/syscall.hpp"

#include "cpu/cpu.hpp"

#include "task/sched.hpp"
#include "task/user.hpp"

#include "io/hpet.hpp"
#include "io/pit.hpp"
#include "io/keyboard.hpp"

#include "mem/pmm.hpp"
#include "mem/vmm.hpp"
#include "mem/heap.hpp"

#include "fs/vfs.hpp"

#include "tty/tty.hpp"

#include "io/serial.hpp"

#include "lib/types.hpp"
#include "lib/errno.hpp"
#include "lib/string.hpp"


// entry stub (cpu/syscall_entry.asm); sysrets back with rax = the value
// returned by syscall_dispatch() below.
extern "C" void syscall_entry();

namespace {

    constexpr u32 IA32_EFER  = 0xC000'0080;
    constexpr u32 IA32_STAR  = 0xC000'0081;
    constexpr u32 IA32_LSTAR = 0xC000'0082;
    constexpr u32 IA32_FMASK = 0xC000'0084;

    constexpr u64 EFER_SCE = 1ULL << 0;  // System Call Extensions

    // must match boot.asm's GDT. STAR[47:32] = kernel CS (SS = +8);
    // STAR[63:48] = user seg base, SYSRET picks user CS at +16 / SS at +8
    // (so 0x10 -> user_data 0x18, user_code 0x20, both DPL=3).
    constexpr u16 SYSCALL_KERNEL_CS_BASE = 0x08;
    constexpr u16 SYSRET_USER_SEG_BASE   = 0x10;

    // --- syscall implementations ---

    // write(fd, buf, len). `buf` is a trusted user pointer for now - no
    // "does this fall in the caller's VMA?" check yet.
    i64 sys_write(u64 fd, const char* buf, u64 len) {
        if (fd != 1 && fd != 2) return -1;
        // stdout+stderr both go to the tty (the on-screen console), mirrored
        // to serial so the debug log still catches program output.
        for (u64 i = 0; i < len; ++i) {
            tty::print(buf[i]);
            serial::print(buf[i]);
        }
        return static_cast<i64>(len);
    }

    [[noreturn]] void sys_exit(u64 code) {
        sched::current_task()->exit_code = code;  // read by a parent's sys_wait4

        // exit_current() jumps to another task via switch_to() and never
        // returns through syscall_entry's tail - so its closing swapgs never
        // runs, and GS_BASE would stay stuck at &g_cpu_local. do the matching
        // swapgs here, or the next task to enter ring 3 inherits a bad GS_BASE
        // and its first syscall reads garbage.
        asm volatile("swapgs" ::: "memory");

        sched::exit_current();  // marks Dead, switches away, never returns
    }

    // writev(fd, iov, iovcnt) - musl's buffered stdio flushes through this,
    // not plain write(). same fd/trust rules, looped over each chunk.
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

    // user page flags for brk/mmap growth. dup of task/user.cpp's USER_RW
    // (that one's file-local in another TU).
    constexpr u64 USER_RW = vmm::PRESENT | vmm::USER | vmm::WRITABLE | vmm::NO_EXECUTE;

    constexpr u64 PAGE_SIZE = 0x1000;
    inline u64 page_down(u64 x) { return x & ~(PAGE_SIZE - 1); }
    inline u64 page_up(u64 x)   { return (x + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1); }

    // must match vmm::create_address_space()'s reserved low window - the only
    // per-task-private slice of PML4[0]. everything else in the low 4GiB is
    // device MMIO / kernel image, shared via 2MB huge-page entries; growing
    // brk past this would split one of those and corrupt every other address
    // space's page tables.
    constexpr u64 BRK_CEILING = 0x0000'0000'0100'0000ULL;  // 16 MiB

    // brk(addr) - always returns the resulting break, never -errno (musl's
    // wrapper compares the return against the request to detect failure).
    u64 sys_brk(u64 addr) {
        task::Task* self = sched::current_task();
        if (addr == 0 || addr < self->brk_start) return self->brk_cur;
        if (addr > BRK_CEILING) return self->brk_cur;

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

    // mmap(addr, len, prot, flags, fd, off) - anonymous-only (musl's malloc
    // arena needs it, not just brk). bump-allocates from Task::mmap_next;
    // munmap frees the frames but never reclaims the VA range - nothing reuses
    // addresses, so no aliasing risk. returns an address or -errno.
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
    // fd 0/1/2 special-cased (no vfs::File backs them); fds 3.. are real
    // vfs::File* in the task's fds[] table. no cwd, so dirfd is ignored -
    // every path resolves from root.

    constexpr u64 STAT_SIZE = 144;  // sizeof(struct stat), x86-64 Linux ABI

    i64 alloc_fd(task::Task* self, vfs::File* f) {
        for (u32 i = 3; i < task::Task::MAX_FDS; ++i) {
            if (!self->fds[i]) { self->fds[i] = f; return static_cast<i64>(i); }
        }
        return -err::EMFILE;
    }

    // real open fd, or null for fd 0/1/2 / out-of-range / closed - one check
    // to tell those apart from a genuinely-open file.
    vfs::File* real_file(task::Task* self, u64 fd) {
        if (fd < 3 || fd >= task::Task::MAX_FDS) return nullptr;
        return self->fds[fd];
    }

    i64 sys_openat(u64 dirfd, const char* path, u64 flags, u64 mode) {
        (void)dirfd; (void)flags; (void)mode;  // no cwd, no writable fs yet
        vfs::File* f = vfs::open(path);
        if (!f) return -err::ENOENT;
        task::Task* self = sched::current_task();
        i64 fd = alloc_fd(self, f);
        if (fd < 0) heap::kfree(f);
        return fd;
    }

    // --- stdin: real, blocking, canonical (line-buffered) mode ---
    // single "line currently being typed" buffer - this kernel only ever
    // has one interactive foreground task at a time in practice (retsh,
    // Phase 8's testbins/retsh.c), so one instance is enough; the whole
    // point of moving line editing here (rather than in retsh itself) is
    // matching what a real tty driver's cooked mode does, AND retiring
    // `kbdecho_task` (main.cpp) as a second, competing consumer of
    // `keyboard::getchar()` - two readers racing the same buffer would
    // split input unpredictably.
    constexpr size_t STDIN_LINE_CAP = 256;
    char stdin_line[STDIN_LINE_CAP];
    size_t stdin_line_len   = 0;      // bytes currently buffered (a full line, once ready)
    size_t stdin_line_pos   = 0;      // how much of it sys_read has already handed out
    bool   stdin_line_ready = false;  // true once '\n' has been appended

    // echoing '\b' via tty::print already erases the screen cell (see tty.cpp's
    // '\b' case), so that's the whole backspace fix-up.
    i64 sys_read_stdin(u8* buf, u64 len) {
        while (!stdin_line_ready) {
            char c = keyboard::getchar();
            if (!c) {
                // nothing typed - block. yield_blocking(), not plain yield();
                // see its doc comment.
                sched::yield_blocking();
                continue;
            }
            if (c == '\b') {
                if (stdin_line_len > 0) {
                    --stdin_line_len;
                    tty::print('\b');
                }
                continue;
            }
            if (stdin_line_len < STDIN_LINE_CAP - 1) {
                stdin_line[stdin_line_len++] = c;
                tty::print(c);
            }
            if (c == '\n') stdin_line_ready = true;
        }

        u64 avail = stdin_line_len - stdin_line_pos;
        u64 n = (len < avail) ? len : avail;
        string::memcpy(buf, stdin_line + stdin_line_pos, n);
        stdin_line_pos += n;
        if (stdin_line_pos >= stdin_line_len) {
            // fully drained - reset for the next line.
            stdin_line_len   = 0;
            stdin_line_pos   = 0;
            stdin_line_ready = false;
        }
        return static_cast<i64>(n);
    }

    i64 sys_read(u64 fd, void* buf, u64 len) {
        if (fd == 0) {
            // save/restore user_rsp around the whole (possibly blocking) call,
            // like sys_wait4 - see yield_blocking()'s doc comment.
            u64 saved_user_rsp = cpu::local()->user_rsp;
            i64 n = sys_read_stdin(reinterpret_cast<u8*>(buf), len);
            cpu::local()->user_rsp = saved_user_rsp;
            return n;
        }
        if (fd == 1 || fd == 2) return -err::EBADF;
        vfs::File* f = real_file(sched::current_task(), fd);
        if (!f) return -err::EBADF;
        ssize_t n = f->inode->ops->read(f->inode, f->offset, buf, len);
        if (n < 0) return -err::EIO;
        f->offset += static_cast<u64>(n);
        return n;
    }

    // getdents64(fd, dirp, count) - translates the VFS dirent format
    // ([u64 inode][NUL name], byte-paged - see vfs.hpp) into Linux
    // linux_dirent64 records (d_ino:8, d_off:8, d_reclen:2, d_type:1, name[]).
    //
    // re-collects the whole (tiny) directory on every call instead of paging:
    // the variable-length records don't align 1:1 with the VFS's read chunks,
    // so partial entries would need holding back without double-emitting.
    // File::offset is repurposed as an entry index (dirs never use it as a
    // byte offset) so repeated calls resume correctly.
    i64 sys_getdents64(u64 fd, u8* dirp, u64 count) {
        vfs::File* f = real_file(sched::current_task(), fd);
        if (!f) return -err::EBADF;
        if (!f->inode->is_dir) return -err::EINVAL;

        struct Ent { u64 ino; char name[48]; };
        constexpr u64 MAX_ENTRIES = 32;
        Ent entries[MAX_ENTRIES];
        u64 num_entries = 0;

        u8 vfs_buf[512];
        u64 vfs_off = 0;
        while (num_entries < MAX_ENTRIES) {
            ssize_t n = f->inode->ops->readdir(f->inode, vfs_off, vfs_buf, sizeof(vfs_buf));
            if (n <= 0) break;
            u8* p = vfs_buf;
            u8* end = vfs_buf + n;
            while (p < end && num_entries < MAX_ENTRIES) {
                u64 ino = *reinterpret_cast<u64*>(p);
                p += 8;
                const char* name = reinterpret_cast<const char*>(p);
                size_t name_len = string::strlen(name);
                p += name_len + 1;

                entries[num_entries].ino = ino;
                string::strncpy(entries[num_entries].name, name, sizeof(entries[num_entries].name) - 1);
                entries[num_entries].name[sizeof(entries[num_entries].name) - 1] = '\0';
                ++num_entries;
            }
            vfs_off += static_cast<u64>(n);
        }

        u64 written = 0;
        u64 idx = f->offset;
        while (idx < num_entries) {
            size_t name_len = string::strlen(entries[idx].name);
            size_t reclen = (19 + name_len + 1 + 7) & ~7ULL;  // header(19) + name + NUL, 8-aligned
            if (written + reclen > count) break;

            u8* out = dirp + written;
            *reinterpret_cast<u64*>(out + 0)  = entries[idx].ino;  // d_ino
            *reinterpret_cast<u64*>(out + 8)  = 0;                 // d_off - unused by callers here
            *reinterpret_cast<u16*>(out + 16) = static_cast<u16>(reclen);  // d_reclen
            out[18] = 0;  // d_type = DT_UNKNOWN - nothing here distinguishes file/dir at this layer
            string::memcpy(out + 19, entries[idx].name, name_len + 1);

            written += reclen;
            ++idx;
        }
        f->offset = idx;
        return static_cast<i64>(written);
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

    // fills the Linux x86-64 struct stat (144 bytes, <bits/stat.h>) by byte
    // offset - the padding's fiddly and it's only used here.
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

    // stat(path, statbuf) - musl on x86-64 issues this for a plain
    // stat()/lstat(), not newfstatat.
    i64 sys_stat(const char* path, u8* statbuf) {
        vfs::File* f = vfs::open(path);
        if (!f) return -err::ENOENT;
        fill_stat(statbuf, f->inode->size, f->inode->is_dir);
        heap::kfree(f);
        return 0;
    }

    // access(path, mode) - existence check only; no permission model to
    // evaluate `mode` against (everything's world-readable - see fill_stat).
    i64 sys_access(const char* path, u64 mode) {
        (void)mode;
        vfs::File* f = vfs::open(path);
        if (!f) return -err::ENOENT;
        heap::kfree(f);
        return 0;
    }

    // dummy but stable, same spirit as getpid/getuid/etc below - 0 for a
    // task with no parent (every `spawn_from_elf` task), the real parent
    // pid for a `sys_fork()`'d one.
    i64 sys_getppid() {
        task::Task* parent = sched::current_task()->parent;
        return parent ? static_cast<i64>(parent->id) : 0;
    }

    // readlink(path, buf, bufsize) - nothing here is ever a symlink, so this
    // just reports existence: EINVAL (exists, not a link) or ENOENT.
    i64 sys_readlink(const char* path, u8* buf, u64 bufsize) {
        (void)buf; (void)bufsize;
        vfs::File* f = vfs::open(path);
        if (!f) return -err::ENOENT;
        heap::kfree(f);
        return -err::EINVAL;
    }

    // getcwd(buf, size) - no cwd tracking; every task's cwd is just "/".
    i64 sys_getcwd(u8* buf, u64 size) {
        constexpr char root[] = "/";
        if (size < sizeof(root)) return -err::EINVAL;
        string::memcpy(buf, root, sizeof(root));
        return sizeof(root);
    }

    constexpr u64 TCGETS = 0x5401;

    i64 sys_ioctl(u64 fd, u64 request, u64 argp) {
        if ((fd == 0 || fd == 1 || fd == 2) && request == TCGETS) {
            // callers only check the return (0 => a tty); nothing reads the
            // termios fields yet, so zeroing suffices.
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

    // uname() - claims to BE "Linux" (not "retux") for binary compat, the
    // same trick WSL1 and BSD's Linuxulator play.
    i64 sys_uname(u8* buf) {
        utsname_field(buf + 0 * 65,   "Linux");
        utsname_field(buf + 1 * 65,   "retux");
        utsname_field(buf + 2 * 65,   "5.0.0");
        utsname_field(buf + 3 * 65,   "retux");
        utsname_field(buf + 4 * 65,   "x86_64");
        utsname_field(buf + 5 * 65,   "");
        return 0;
    }

    // no wall clock - just uptime from whichever timer main.cpp picked. the
    // ticks() > 0 check avoids hpet::milliseconds() dividing by an unset
    // configured_hz when PIT is the active timer.
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

// called from the entry stub with the Frame it built; return goes into rax.
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
        case SYS_GETDENTS64:
            return static_cast<u64>(sys_getdents64(f->arg0, reinterpret_cast<u8*>(f->arg1), f->arg2));
        case SYS_FSTAT:
            return static_cast<u64>(sys_fstat(f->arg0, reinterpret_cast<u8*>(f->arg1)));
        case SYS_STAT:
            return static_cast<u64>(sys_stat(reinterpret_cast<const char*>(f->arg0), reinterpret_cast<u8*>(f->arg1)));
        case SYS_ACCESS:
            return static_cast<u64>(sys_access(reinterpret_cast<const char*>(f->arg0), f->arg1));
        case SYS_GETPPID:
            return static_cast<u64>(sys_getppid());
        case SYS_READLINK:
            return static_cast<u64>(sys_readlink(
                reinterpret_cast<const char*>(f->arg0),
                reinterpret_cast<u8*>(f->arg1),
                f->arg2));
        case SYS_GETCWD:
            return static_cast<u64>(sys_getcwd(reinterpret_cast<u8*>(f->arg0), f->arg1));
        case SYS_LSEEK:
            return static_cast<u64>(sys_lseek(f->arg0, static_cast<i64>(f->arg1), f->arg2));
        case SYS_IOCTL:
            return static_cast<u64>(sys_ioctl(f->arg0, f->arg1, f->arg2));
        case SYS_OPENAT:
            return static_cast<u64>(sys_openat(f->arg0, reinterpret_cast<const char*>(f->arg1), f->arg2, f->arg3));
        case SYS_OPEN:  // open(path,...) == openat(ignored dirfd, path, ...)
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
        case SYS_FORK:
            return static_cast<u64>(task::user::sys_fork(f));
        case SYS_EXECVE:
            return static_cast<u64>(task::user::sys_execve(
                reinterpret_cast<const char*>(f->arg0),
                reinterpret_cast<char* const*>(f->arg1),
                reinterpret_cast<char* const*>(f->arg2)));
        case SYS_WAIT4:
            return static_cast<u64>(sched::wait4(
                static_cast<i64>(f->arg0),
                reinterpret_cast<u32*>(f->arg1)));
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
            return 1;  // fake but stable tid - tid tracking is moot single-threaded
        case SYS_SET_ROBUST_LIST:
        case SYS_RT_SIGACTION:
        case SYS_RT_SIGPROCMASK:
            return 0;  // no-ops - enough for musl's single-threaded startup
        case SYS_EXIT:
        case SYS_EXIT_GROUP:
            sys_exit(f->arg0);
            // unreachable - sys_exit is [[noreturn]]
        default:
            // log the number rather than silently misbehaving - drives which
            // syscall gets implemented next.
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

        // FMASK: these bits get cleared in RFLAGS on entry. clearing IF keeps
        // interrupts off across the entry stub's swapgs->kernel-stack window
        // (an IRQ there would run on the user RSP); clearing DF is the SysV
        // ABI requirement for kernel string ops.
        constexpr u64 RFLAGS_IF = 1ULL << 9;
        constexpr u64 RFLAGS_DF = 1ULL << 10;
        cpu::wrmsr(IA32_FMASK, RFLAGS_IF | RFLAGS_DF);

        // EFER.SCE - off by default even in long mode; SYSCALL #UDs without it.
        u64 efer = cpu::rdmsr(IA32_EFER);
        cpu::wrmsr(IA32_EFER, efer | EFER_SCE);

        serial::print("syscall: SYSCALL/SYSRET enabled\n");
    }

}
