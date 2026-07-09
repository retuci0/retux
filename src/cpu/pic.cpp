#include "cpu/pic.hpp"

#include "lib/port.hpp"
#include "lib/types.hpp"


namespace {

    constexpr u16 PIC1_CMD  = 0x20;
    constexpr u16 PIC1_DATA = 0x21;
    constexpr u16 PIC2_CMD  = 0xA0;
    constexpr u16 PIC2_DATA = 0xA1;

}


namespace pic {

    void disable() {
        // ICW1: start the init sequence on both PICs, tell them ICW4 is coming
        port::outb(PIC1_CMD, 0x11); port::io_wait();
        port::outb(PIC2_CMD, 0x11); port::io_wait();

        // ICW2: vector offsets. IRQ0-7 -> 0x20-0x27, IRQ8-15 -> 0x28-0x2F.
        // this matters even though we mask everything below: an un-remapped
        // PIC still defaults to firing IRQ0-7 as vectors 0x08-0x0F, which
        // land right on top of #DF, #TS, #NP, etc.
        port::outb(PIC1_DATA, 0x20); port::io_wait();
        port::outb(PIC2_DATA, 0x28); port::io_wait();

        // ICW3: describe the cascade wiring (slave PIC hangs off IRQ2 of the master)
        port::outb(PIC1_DATA, 0x04); port::io_wait();
        port::outb(PIC2_DATA, 0x02); port::io_wait();

        // ICW4: 8086/8088 mode
        port::outb(PIC1_DATA, 0x01); port::io_wait();
        port::outb(PIC2_DATA, 0x01); port::io_wait();

        // mask every IRQ line on both PICs - the I/O APIC owns interrupt
        // routing from here on.
        port::outb(PIC1_DATA, 0xFF);
        port::outb(PIC2_DATA, 0xFF);
    }

}
