#pragma once


namespace pic {

    // remap the 8259 PIC pair off vectors 0-15 (which collide with CPU
    // exceptions) onto 0x20-0x2F, then mask every line. needed even when using
    // only the APIC - a PIC left at its power-on offsets can fire into the
    // exception range the moment interrupts are enabled.
    void disable();

}
