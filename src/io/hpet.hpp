#pragma once

#include "lib/types.hpp"


// High Precision Event Timer, driven through the "legacy replacement"
// route so it lands on IRQ0 - the exact same I/O APIC pin the PIT would
// use.
// requires acpi::init() to have found an ACPI "HPET" table with legacy
// replacement support (LEG_RT_CAP)

namespace hpet {

    // true if acpi::init() found a usable HPET table. does not by itself
    // mean init() will succeed - init() also needs LEG_RT_CAP support.
    bool available();

    // maps the HPET's MMIO page, programs timer 0 for a periodic interrupt
    // at approximately `freq` Hz, registers its IRQ0 handler, and
    // unmasks IRQ0 at the I/O APIC. call after apic::init_ioapic() +
    // irq::init(). do not call alongside pit::init() - both would drive
    // IRQ0 at once. returns false (and touches nothing) if unavailable.
    bool init(u32 freq);

    // ticks since init() - incremented once per interrupt.
    u64 ticks();

    // milliseconds elapsed since init(), derived from the HPET's own
    // counter period - accurate to the HPET's actual clock, unlike the
    // PIT's rounded divisor.
    u64 milliseconds();

    // optional callback invoked at the end of every tick, from IRQ context,
    // BEFORE `apic::eoi()` has been sent for this interrupt - keep it fast
    // and non-blocking (e.g. `sched::tick()`, which only touches a counter
    // and a flag). pass nullptr to clear it.
    using TickCallback = void (*)();
    void set_tick_callback(TickCallback cb);

}
