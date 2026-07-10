#pragma once

#include "lib/types.hpp"


// dispatch layer for hardware IRQs (vectors 0x20-0x2F), separate from the
// CPU-exception handling in `idt.cpp`/`isr.asm` even though the stubs are
// near-identical in shape.

namespace irq {

    // byte-identical to `isr.asm`'s push order - both stubs push the same
    // register set before calling into C++.
    struct Frame {
        u64 r15, r14, r13, r12, r11, r10, r9, r8;
        u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
        u64 vector;
        u64 error_code;  // always 0 for IRQs; kept so the frame lines up with `idt.cpp`'s
        u64 rip;
        u64 cs;
        u64 rflags;
        u64 rsp;
        u64 ss;
    } __attribute__((packed));

    using Handler = void (*)(Frame*);

    // installs IDT gates 32-47 pointing at the IRQ stubs in `isr.asm`.
    // call after idt::init() (needs idt::install_gate()) and before sti.
    void init();

    // register a handler for legacy ISA IRQ `n` (0-15). only one handler
    // per line - registering again replaces the previous one. remember to
    // also call `apic::set_irq_mask(n, false)` once you've registered it,
    // since init_ioapic() starts every line masked.
    void register_handler(u8 n, Handler handler);

    // optional hook run at the very end of every IRQ dispatch, right after
    // `apic::eoi()`. exists so subsystems that need to act on interrupts
    // but must never delay sending EOI (e.g. the scheduler deciding
    // whether to preempt) can do so safely - by the time this runs, the
    // local APIC has already been told it's free to deliver the next one.
    // only one hook at a time; pass nullptr to clear it.
    using PostEoiHook = void (*)();
    void set_post_eoi_hook(PostEoiHook hook);

}
