#include "cpu/cpu.hpp"

#include "lib/types.hpp"


namespace {

    // static-storage-duration: initialised to zero before kernel_main runs.
    // one instance is enough for now - single CPU, single per-CPU area.
    cpu::CpuLocal g_cpu_local;

    constexpr u32 IA32_KERNEL_GS_BASE = 0xC000'0102;

}

namespace cpu {

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

    void init() {
        wrmsr(IA32_KERNEL_GS_BASE, reinterpret_cast<u64>(&g_cpu_local));
        // GS_BASE itself is left at 0 - the boot task never references
        // [gs:] on its own, so its value doesn't matter until the first
        // `swapgs` in the syscall entry stub flips them.
    }

    CpuLocal* local() {
        return &g_cpu_local;
    }

}
