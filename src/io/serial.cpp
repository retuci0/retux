#include "io/serial.hpp"


void serial::init() {
    port::outb(port + 1, 0x00);  // disable interrupts on the UART
    port::outb(port + 3, 0x80);  // enable DLAB to set the baud-rate divisor
    port::outb(port + 0, 0x03);  // divisor low byte -> 38400 baud
    port::outb(port + 1, 0x00);  // divisor high byte
    port::outb(port + 3, 0x03);  // 8 data bits, no parity, 1 stop bit; DLAB off
    port::outb(port + 2, 0xC7);  // enable FIFO, clear it, 14-byte trigger level
    port::outb(port + 4, 0x0B);  // mark data-terminal-ready, request-to-send
}

void serial::print(char c) {
    while (!transmit_ready()) {
        // spin until the UART's transmit buffer has room
    }
    port::outb(port, c);
}

void serial::print(const char* str) {
    while (*str) {
        print(*str++);
    }
}
