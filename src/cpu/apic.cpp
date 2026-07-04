#include "cpu/apic.hpp"

#include "boot/acpi.hpp"

#include "memory/vmm.hpp"

#include "lib/types.hpp"
#include "lib/hex.hpp"

#include "io/serial.hpp"


namespace {

    constexpr u32 IA32_APIC_BASE_MSR = 0x1B;

    // local APIC register offsets (each register is a 32-bit window into
    // a 128-bit-aligned slot; we only ever touch the low 32 bits)
    constexpr u32 LAPIC_REG_ID  = 0x020;
    constexpr u32 LAPIC_REG_TPR = 0x080;  // task priority - 0 accepts everything
    constexpr u32 LAPIC_REG_EOI = 0x0B0;
    constexpr u32 LAPIC_REG_SVR = 0x0F0;  // spurious interrupt vector register

    constexpr u32 LAPIC_REG_TIMER_LVT  = 0x320;  // vector + mode for the timer's own LVT entry
    constexpr u32 LAPIC_REG_TIMER_INIT = 0x380;  // writing this starts the countdown
    constexpr u32 LAPIC_REG_TIMER_DIV  = 0x3E0;  // divides the bus clock feeding the timer

    // I/O APIC register offsets. registers are accessed indirectly: write
    // the register index to REGSEL, then read/write the value at REGWIN.
    constexpr u32 IOAPIC_REGSEL  = 0x00;
    constexpr u32 IOAPIC_REGWIN  = 0x10;
    constexpr u32 IOAPIC_REG_VER = 0x01;
    constexpr u32 IOAPIC_REDTBL  = 0x10;  // entry n lives at REDTBL + 2n (low) / +2n+1 (high)

    u64 lapic_virt  = 0;  // == phys (identity-mapped)
    u64 ioapic_virt = 0;
    u32 ioapic_gsi_base = 0;
    u32 ioapic_max_entry = 0;

    u64 rdmsr(u32 msr) {
        u32 lo, hi;
        asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
        return (static_cast<u64>(hi) << 32) | lo;
    }

    void wrmsr(u32 msr, u64 value) {
        u32 lo = static_cast<u32>(value & 0xFFFFFFFF);
        u32 hi = static_cast<u32>(value >> 32);
        asm volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(msr));
    }

    u32 lapic_read(u32 reg) {
        return *reinterpret_cast<volatile u32*>(lapic_virt + reg);
    }

    void lapic_write(u32 reg, u32 value) {
        *reinterpret_cast<volatile u32*>(lapic_virt + reg) = value;
    }

    u32 ioapic_read(u32 reg) {
        *reinterpret_cast<volatile u32*>(ioapic_virt + IOAPIC_REGSEL) = reg;
        return *reinterpret_cast<volatile u32*>(ioapic_virt + IOAPIC_REGWIN);
    }

    void ioapic_write(u32 reg, u32 value) {
        *reinterpret_cast<volatile u32*>(ioapic_virt + IOAPIC_REGSEL) = reg;
        *reinterpret_cast<volatile u32*>(ioapic_virt + IOAPIC_REGWIN) = value;
    }

    // identity-map one MMIO page, marked uncacheable (PWT|PCD) - reading a
    // stale cached value from a device register is a classic APIC bug.
    void map_mmio(u64 addr) {
        vmm::map(addr, addr, vmm::KERNEL_RW | vmm::PWT | vmm::PCD);
    }

    // returns the redirection-table register for legacy IRQ `irq`, or -1
    // if that IRQ's GSI doesn't fall on the I/O APIC we mapped (only
    // relevant on multi-I/O-APIC boards, which `init_ioapic()` below doesn't
    // fully handle - see its comment).
    i32 redir_reg_for_irq(u8 irq) {
        u32 gsi = acpi::irq_to_gsi(irq);
        if (gsi < ioapic_gsi_base || gsi - ioapic_gsi_base > ioapic_max_entry) {
            return -1;
        }
        return static_cast<i32>(IOAPIC_REDTBL + (gsi - ioapic_gsi_base) * 2);
    }

}

namespace apic {

    void init_lapic() {
        u64 phys = acpi::lapic_address();
        map_mmio(phys);
        lapic_virt = phys;

        // point the MSR at ACPI's address (normally already correct) and
        // make sure the global enable bit (11) is set.
        u64 base = rdmsr(IA32_APIC_BASE_MSR);
        base &= ~0xFFFULL;
        base |= phys;
        base |= (1ULL << 11);
        wrmsr(IA32_APIC_BASE_MSR, base);

        lapic_write(LAPIC_REG_TPR, 0);  // accept every priority level

        // bit 8 here is the APIC's own software enable switch (separate
        // from the MSR enable bit above). vector 0xFF is the conventional
        // spurious-interrupt vector - pick something that isn't used for
        // anything else.
        lapic_write(LAPIC_REG_SVR, 0x1FF);

        char buf[17];
        serial::print("apic: local APIC enabled, id=0x");
        hex::to_string(lapic_read(LAPIC_REG_ID) >> 24, buf);
        serial::print(buf);
        serial::print("\n");
    }

    void eoi() {
        lapic_write(LAPIC_REG_EOI, 0);
    }

    void init_ioapic() {
        if (acpi::ioapic_count() == 0) {
            serial::print("apic: no I/O APIC to configure\n");
            return;
        }

        const auto& io = acpi::ioapic(0);
        map_mmio(io.address);
        ioapic_virt     = io.address;
        ioapic_gsi_base = io.gsi_base;
        ioapic_max_entry = (ioapic_read(IOAPIC_REG_VER) >> 16) & 0xFF;

        u8 dest_apic_id = static_cast<u8>(lapic_read(LAPIC_REG_ID) >> 24);

        for (u8 irq = 0; irq < 16; ++irq) {
            i32 reg = redir_reg_for_irq(irq);
            if (reg < 0) continue;  // this IRQ's GSI isn't on this I/O APIC

            // low dword: vector + masked. delivery mode 0 (fixed),
            // destination mode 0 (physical), polarity/trigger left at their
            // power-on default (active-high, edge-triggered - correct for
            // ISA IRQs that don't have an override saying otherwise).
            u32 low  = (IRQ_BASE + irq) | (1u << 16);
            u32 high = static_cast<u32>(dest_apic_id) << 24;

            ioapic_write(static_cast<u32>(reg),     low);
            ioapic_write(static_cast<u32>(reg) + 1, high);
        }

        serial::print("apic: I/O APIC configured, all IRQs masked\n");
    }

    void start_test_timer(u8 vector, u32 count) {
        lapic_write(LAPIC_REG_TIMER_DIV, 0x3);       // divide bus clock by 16
        lapic_write(LAPIC_REG_TIMER_LVT, vector);    // one-shot (bit 17 = 0), unmasked (bit 16 = 0)
        lapic_write(LAPIC_REG_TIMER_INIT, count);    // starts counting down immediately
    }

    void set_irq_mask(u8 irq, bool masked) {
        if (!ioapic_virt) return;
        i32 reg = redir_reg_for_irq(irq);
        if (reg < 0) return;

        u32 low = ioapic_read(static_cast<u32>(reg));
        if (masked) low |= (1u << 16);
        else low &= ~(1u << 16);
        ioapic_write(static_cast<u32>(reg), low);
    }

}
