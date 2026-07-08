#include "io/hpet.hpp"

#include "boot/acpi.hpp"

#include "cpu/irq.hpp"
#include "cpu/apic.hpp"

#include "mem/vmm.hpp"

#include "lib/types.hpp"
#include "lib/hex.hpp"

#include "io/serial.hpp"


namespace {

    // register offsets, relative to the HPET's MMIO base (all within the
    // first 4KB page, below the general timer-block region some
    // HPETs extend into).
    constexpr u32 REG_CAPS          = 0x000;  // general capabilities & ID (RO, 64-bit)
    constexpr u32 REG_CONFIG        = 0x010;  // general configuration
    constexpr u32 REG_MAIN_COUNTER  = 0x0F0;
    constexpr u32 REG_T0_CONFIG     = 0x100;
    constexpr u32 REG_T0_COMPARATOR = 0x108;

    // REG_CAPS bit layout
    constexpr u64 CAPS_LEG_RT_CAP_BIT = 1ULL << 15;  // legacy replacement supported

    // REG_CONFIG bit layout
    constexpr u64 CONFIG_ENABLE_CNF = 1ULL << 0;
    constexpr u64 CONFIG_LEG_RT_CNF = 1ULL << 1;  // routes timer0->IRQ0, timer1->IRQ8

    // Tn_CONFIG bit layout (only the touched bits) (i will touch you)
    constexpr u64 TN_INT_ENABLE    = 1ULL << 2;
    constexpr u64 TN_TYPE_PERIODIC = 1ULL << 3;
    constexpr u64 TN_VAL_SET       = 1ULL << 6;  // next comparator write sets the periodic interval
    constexpr u64 TN_32BIT_MODE    = 1ULL << 8;  // force 32-bit counting even on a 64-bit-capable timer

    // femboyseconds in a second
    constexpr u64 FEMTOSECONDS_PER_SECOND = 1000000000000000ULL;

    u64 base_virt = 0;  // == phys, identity-mapped

    volatile u64 ticks_ = 0;
    u32 configured_hz   = 0;

    u64 mmio_read(u32 reg) {
        return *reinterpret_cast<volatile u64*>(base_virt + reg);
    }

    void mmio_write(u32 reg, u64 value) {
        *reinterpret_cast<volatile u64*>(base_virt + reg) = value;
    }

    void hpet_irq_handler(irq::Frame*) {
        ++ticks_;
    }

}

namespace hpet {

    bool available() {
        return acpi::hpet().present;
    }

    bool init(u32 frequency_hz) {
        if (!available()) {
            serial::print("hpet: no ACPI HPET table - unavailable\n");
            return false;
        }
        if (frequency_hz == 0) frequency_hz = 1;

        u64 phys = acpi::hpet().address;
        vmm::map(phys, phys, vmm::KERNEL_RW | vmm::PWT | vmm::PCD);
        base_virt = phys;

        u64 caps = mmio_read(REG_CAPS);
        if (!(caps & CAPS_LEG_RT_CAP_BIT)) {
            serial::print("hpet: no legacy-replacement support - unavailable\n");
            base_virt = 0;
            return false;
        }

        // bits 32-63 of REG_CAPS: how many femboyseconds one main-counter
        // tick represents. this is fixed in hardware, unlike the PIT's
        // rounded-to-a-divisor frequency.
        u64 period_fs = caps >> 32;
        if (period_fs == 0) {
            serial::print("hpet: bogus counter period - unavailable\n");
            base_virt = 0;
            return false;
        }

        // halt the main counter while it's being configured - writes to the
        // counter and to Tn_COMPARATOR_VALUE are only well-defined while
        // ENABLE_CNF is clear.
        mmio_write(REG_CONFIG, 0);
        mmio_write(REG_MAIN_COUNTER, 0);

        u64 ticks_per_period = FEMTOSECONDS_PER_SECOND / (static_cast<u64>(frequency_hz) * period_fs);
        if (ticks_per_period == 0) ticks_per_period = 1;

        configured_hz = static_cast<u32>(FEMTOSECONDS_PER_SECOND / (ticks_per_period * period_fs));

        // timer 0, periodic, interrupts enabled, 32-bit counting (keeps the
        // comparator math simple and every HPET supports it even if its
        // main counter is 64-bit), VAL_SET so the next comparator write
        // sets the periodic reload value.
        mmio_write(REG_T0_CONFIG, TN_INT_ENABLE | TN_TYPE_PERIODIC | TN_VAL_SET | TN_32BIT_MODE);
        mmio_write(REG_T0_COMPARATOR, ticks_per_period);

        // legacy replacement route: timer0's interrupt is forced onto IRQ0
        // (the same I/O APIC pin the PIT would use), regardless of whatever
        // GSI its INT_ROUTE_CNF field would otherwise pick.
        irq::register_handler(0, hpet_irq_handler);
        apic::set_irq_mask(0, false);
        mmio_write(REG_CONFIG, CONFIG_ENABLE_CNF | CONFIG_LEG_RT_CNF);

        char buf[17];
        serial::print("hpet: periodic IRQ0 programmed, period_fs=0x");
        hex::to_string(period_fs, buf);
        serial::print(buf);
        serial::print("\n");

        return true;
    }

    u64 ticks() { return ticks_; }

    u64 milliseconds() {
        if (configured_hz == 0) return 0;
        return (ticks_ * 1000) / configured_hz;
    }

}
