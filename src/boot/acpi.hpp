#pragma once

#include "lib/types.hpp"


// walks the ACPI tables the bootloader handed us (via the Multiboot2 RSDP
// tags) far enough to find the MADT, which is the only table the APIC
// setup actually needs: it gives us the local APIC's MMIO address, every
// I/O APIC's MMIO address + GSI base, and any legacy-IRQ remaps.

namespace acpi {

    constexpr int MAX_IOAPICS = 4;

    struct IoApic {
        u64 address;
        u32 gsi_base;
    };

    struct HpetInfo {
        bool present = false;
        u64  address  = 0;  // MMIO base
    };

    // parse RSDP -> RSDT/XSDT -> MADT. must be called before apic::init_lapic()
    // or apic::init_ioapic(). returns false if no usable ACPI info or no
    // I/O APIC was found (in which case the APIC path can't be used).
    bool init(u64 boot_info_addr);

    u64 lapic_address();

    int ioapic_count();
    const IoApic& ioapic(int index);

    // present() is false if the machine has no HPET table at all (common on
    // older/virtualized hardware) - hpet::init() checks this for you.
    const HpetInfo& hpet();

    // resolve a legacy ISA IRQ (0-15, e.g. keyboard = 1, PIT = 0) to the
    // GSI it's actually wired to. usually GSI == IRQ, but interrupt source
    // overrides in the MADT can remap this (PIT -> GSI 2 is common).
    u32 irq_to_gsi(u8 irq);

}
