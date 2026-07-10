#include "cpu/syscall.hpp"

#include "task/sched.hpp"

#include "tty/tty.hpp"

#include "io/serial.hpp"

#include "lib/types.hpp"


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

    u64 rdmsr(u32 msr) {
        u32 low, high;
        asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
        return (static_cast<u64>(high) << 32) | low;
    }

    void wrmsr(u32 msr, u64 value) {
        u32 low  = static_cast<u32>(value & 0xFFFFFFFF);
        u32 high = static_cast<u32>(value >> 32);
        asm volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
    }

    // ---- syscall implementations ----

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
        (void)code;  // not surfaced anywhere yet
        sched::exit_current();  // marks Dead, switches away, never returns
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
        case SYS_EXIT:
            sys_exit(f->arg0);
            // unreachable - sys_exit is [[noreturn]]
        default:
            return static_cast<u64>(-1);
    }
}

namespace syscall {

    void init() {
        // STAR: high 32 bits pick the CS/SS pairs SYSCALL / SYSRET use.
        u64 star = (static_cast<u64>(SYSRET_USER_SEG_BASE)   << 48) |
                   (static_cast<u64>(SYSCALL_KERNEL_CS_BASE) << 32);
        wrmsr(IA32_STAR, star);

        // LSTAR: the address SYSCALL jumps to in 64-bit mode.
        wrmsr(IA32_LSTAR, reinterpret_cast<u64>(&syscall_entry));

        // FMASK: bits set here get CLEARED in RFLAGS on SYSCALL entry.
        // clearing IF disables interrupts across the CS/RSP swap window
        // in the entry stub - otherwise an IRQ arriving between "swapgs"
        // and "switch to kernel stack" would run with user RSP and blow
        // straight through some untrusted address. clearing DF keeps
        // System V ABI happy (kernel string ops assume DF=0).
        constexpr u64 RFLAGS_IF = 1ULL << 9;
        constexpr u64 RFLAGS_DF = 1ULL << 10;
        wrmsr(IA32_FMASK, RFLAGS_IF | RFLAGS_DF);

        // finally, actually turn SYSCALL/SYSRET on. up till now they'd
        // #UD - EFER.SCE is off by default even in long mode.
        u64 efer = rdmsr(IA32_EFER);
        wrmsr(IA32_EFER, efer | EFER_SCE);

        serial::print("syscall: SYSCALL/SYSRET enabled\n");
    }

}
