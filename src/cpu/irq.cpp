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
    irq::PostEoiHook post_eoi_hook = nullptr;
}


namespace irq {

    void init() {
        for (u8 i = 0; i < 16; ++i) {
            idt::install_gate(apic::IRQ_BASE + i, irq_stub_table[i]);
        }
    }

    void register_handler(u8 n, Handler handler) {
        if (n < 16) handlers[n] = handler;
    }

    void set_post_eoi_hook(PostEoiHook hook) {
        post_eoi_hook = hook;
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

    // safe to do arbitrarily expensive/stack-switching work from here on -
    // EOI has already been sent, so nothing here can delay the next
    // interrupt on this (or any) vector.
    if (post_eoi_hook) post_eoi_hook();
}
