#include "cpu/pic.hpp"

#include "lib/types.hpp"

#include "io/serial.hpp"


namespace {

    constexpr u16 PIC1_CMD  = 0x20;
    constexpr u16 PIC1_DATA = 0x21;
    constexpr u16 PIC2_CMD  = 0xA0;
    constexpr u16 PIC2_DATA = 0xA1;

    void outb(u16 port, u8 val) {
        asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
    }

    // a throwaway write to an unused port. real 8259 hardware needs a small
    // delay between successive command writes
    // this is the traditional way
    // to get one without an actual timer.
    void io_wait() {
        outb(0x80, 0);
    }

}


namespace pic {

    void disable() {
        // ICW1: start the init sequence on both PICs, tell them ICW4 is coming
        outb(PIC1_CMD, 0x11); io_wait();
        outb(PIC2_CMD, 0x11); io_wait();

        // ICW2: vector offsets. IRQ0-7 -> 0x20-0x27, IRQ8-15 -> 0x28-0x2F.
        // this matters even though we mask everything below: an un-remapped
        // PIC still defaults to firing IRQ0-7 as vectors 0x08-0x0F, which
        // land right on top of #DF, #TS, #NP, etc.
        outb(PIC1_DATA, 0x20); io_wait();
        outb(PIC2_DATA, 0x28); io_wait();

        // ICW3: describe the cascade wiring (slave PIC hangs off IRQ2 of the master)
        outb(PIC1_DATA, 0x04); io_wait();
        outb(PIC2_DATA, 0x02); io_wait();

        // ICW4: 8086/8088 mode
        outb(PIC1_DATA, 0x01); io_wait();
        outb(PIC2_DATA, 0x01); io_wait();

        // mask every IRQ line on both PICs - the I/O APIC owns interrupt
        // routing from here on.
        outb(PIC1_DATA, 0xFF);
        outb(PIC2_DATA, 0xFF);

        serial::print("pic: remapped to 0x20-0x2F and fully masked\n");
    }

}
