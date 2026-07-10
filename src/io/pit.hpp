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

    // optional callback invoked at the end of every tick, from IRQ context,
    // BEFORE `apic::eoi()` has been sent for this interrupt - keep it fast
    // and non-blocking (e.g. `sched::tick()`, which only touches a counter
    // and a flag). pass nullptr to clear it.
    using TickCallback = void (*)();
    void set_tick_callback(TickCallback cb);

}
