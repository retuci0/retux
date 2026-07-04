#pragma once

#include "lib/types.hpp"

// classic PIT (Intel 8253/8254) driven off IRQ0 through the I/O APIC.
// simple, universally supported, but low resolution and imprecise at high frequencies.
// use `hpet::` instead when available.
namespace pit {

    constexpr u32 BASE_FREQ = 1193192;  // Hz

    // call after `apic::init_ioapic()` + `irq::init()`.
    // do NOT call alongside `hpet::init()`, as they share the same IRQ.
    void init(u32 freq);

    u64 ticks();  // ticks since `init()`. increments once per interrupt.

    u64 milliseconds();  // milliseconds since `init()`. not very precise.

}
