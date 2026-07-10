#include "io/pit.hpp"

#include "cpu/irq.hpp"
#include "cpu/apic.hpp"

#include "lib/port.hpp"
#include "lib/types.hpp"


namespace {

    constexpr u16 PIT_CHANNEL0 = 0x40;
    constexpr u16 PIT_COMMAND  = 0x43;

    volatile u64 ticks_ = 0;
    u32 configured_hz = 0;

    pit::TickCallback tick_cb = nullptr;

    void pit_irq_handler(irq::Frame*) {
        ++ticks_;
        if (tick_cb) tick_cb();
    }

}


namespace pit {

    void init(u32 frequency_hz) {
        if (frequency_hz == 0) frequency_hz = 1;

        // the PIT counts down from this divisor at `BASE_FREQ` and fires
        // once it hits zero, reloading automatically in mode 3.
        u32 divisor = BASE_FREQ / frequency_hz;
        if (divisor == 0) divisor = 1;           // cap requested frequency at BASE_FREQ
        if (divisor > 0xFFFF) divisor = 0xFFFF;  // 16-bit counter - 0 in the register means 65536

        configured_hz = BASE_FREQ / divisor;  // the frequency we actually got, rounded

        // channel 0, lobyte/hibyte access mode, mode 3 (square wave
        // generator - the usual choice for a periodic tick), binary mode.
        port::outb(PIT_COMMAND, 0x36);
        port::outb(PIT_CHANNEL0, static_cast<u8>(divisor & 0xFF));
        port::outb(PIT_CHANNEL0, static_cast<u8>((divisor >> 8) & 0xFF));

        irq::register_handler(0, pit_irq_handler);
        apic::set_irq_mask(0, false);
    }

    u64 ticks() { return ticks_; }

    u64 milliseconds() {
        if (configured_hz == 0) return 0;
        return (ticks_ * 1000) / configured_hz;
    }

    void set_tick_callback(TickCallback cb) {
        tick_cb = cb;
    }

}
