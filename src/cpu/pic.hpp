#pragma once


namespace pic {

    // remap the legacy 8259 PIC pair off of vectors 0-15 (where they
    // collide head-on with CPU exceptions) onto 0x20-0x2F, then mask every
    // line. must be called even when only ever using the APIC: a PIC that's
    // still sitting at its power-on vector offsets can fire straight into
    // the exception range the moment interrupts are enabled, before the
    // I/O APIC has taken over routing.
    void disable();

}
