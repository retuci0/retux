#include "cpu/irq.hpp"
#include "cpu/idt.hpp"
#include "cpu/apic.hpp"

#include "lib/hex.hpp"

#include "io/serial.hpp"


// 16 IRQ stub addresses (vectors 32-47), defined in `isr.asm` right after
// the 32 exception stubs.
extern "C" u64 irq_stub_table[16];

namespace {
    irq::Handler handlers[16] = {};
}


namespace irq {

    void init() {
        for (int i = 0; i < 16; ++i) {
            idt::install_gate(apic::IRQ_BASE + i, irq_stub_table[i]);
        }
    }

    void register_handler(u8 n, Handler handler) {
        if (n < 16) handlers[n] = handler;
    }

}

extern "C" void irq_common_handler(irq::Frame* frame) {
    u8 n = static_cast<u8>(frame->vector - apic::IRQ_BASE);

    if (n < 16 && handlers[n]) {
        handlers[n](frame);
    } else {
        serial::print("irq: unhandled IRQ ");
        char buf[17];
        hex::to_string(n, buf);
        serial::print(buf);
        serial::print("\n");
    }

    // must run after every IRQ, handled or not - the local APIC won't
    // deliver another interrupt on this (or any lower-priority) vector
    // until it sees this.
    apic::eoi();
}
