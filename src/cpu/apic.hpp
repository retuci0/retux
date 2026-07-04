#pragma once

#include "lib/types.hpp"


namespace apic {

    // IRQ0 -> vector 0x20, IRQ1 -> 0x21, ... IRQ15 -> 0x2F.
    // chosen to line up with where the (now-disabled) PIC used to put them,
    // so nothing else has to change if you're used to that numbering.
    constexpr u8 IRQ_BASE = 0x20;

    // map the local APIC's MMIO page, point the APIC_BASE MSR at it, and
    // enable it via the spurious-interrupt vector register.
    // requires acpi::init() to have already found the local APIC address.
    void init_lapic();

    // signal end-of-interrupt to the local APIC. every IRQ handler must
    // call this once it's done (in this setup, irq_common_handler in
    // irq.cpp calls it automatically after dispatch).
    void eoi();

    // map the (first) I/O APIC found by acpi::init() and program its
    // redirection table so legacy ISA IRQs 0-15 deliver to vectors
    // IRQ_BASE..IRQ_BASE+15 on the local APIC that called this function.
    // every entry starts masked; use set_irq_mask() to turn one on once
    // you've installed a handler for it.
    void init_ioapic();

    // (un)mask a single legacy ISA IRQ line (0-15) at the I/O APIC.
    void set_irq_mask(u8 irq, bool masked);

    // arms the local APIC's own timer for a single one-shot interrupt on
    // `vector`, firing after roughly `count` ticks (divide-by-16). exists
    // purely as a self-test: the LAPIC timer is delivered directly by the
    // local APIC, so it proves init_lapic()+irq::init()+eoi() all work
    // without depending on the I/O APIC or any real device being present.
    void start_test_timer(u8 vector, u32 count);

}
