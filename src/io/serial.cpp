#include "io/serial.hpp"


void serial::outb(u16 p, u8 val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(p));
}

u8 serial::inb(u16 p) {
    u8 ret;
    asm volatile("inb %1, %0" : "=a"(ret) : "Nd"(p));
    return ret;
}

void serial::init() {
    outb(port + 1, 0x00);  // disable interrupts on the UART
    outb(port + 3, 0x80);  // enable DLAB to set the baud-rate divisor
    outb(port + 0, 0x03);  // divisor low byte -> 38400 baud
    outb(port + 1, 0x00);  // divisor high byte
    outb(port + 3, 0x03);  // 8 data bits, no parity, 1 stop bit; DLAB off
    outb(port + 2, 0xC7);  // enable FIFO, clear it, 14-byte trigger level
    outb(port + 4, 0x0B);  // mark data-terminal-ready, request-to-send
}

void serial::print(char c) {
    while (!transmit_ready()) {
        // spin until the UART's transmit buffer has room
    }
    outb(port, c);
}

void serial::print(const char* str) {
    while (*str) {
        print(*str++);
    }
}
