#pragma once

#include "types.h"


namespace serial {

    constexpr u16 port = 0x3F8;  // COM1

    inline void outb(u16 p, u8 val) {
        asm volatile("outb %0, %1" : : "a"(val), "Nd"(p));
    }

    inline u8 inb(u16 p) {
        u8 ret;
        asm volatile("inb %1, %0" : "=a"(ret) : "Nd"(p));
        return ret;
    }

    inline void init() {
        outb(port + 1, 0x00);  // disable interrupts on the UART
        outb(port + 3, 0x80);  // enable DLAB to set the baud-rate divisor
        outb(port + 0, 0x03);  // divisor low byte -> 38400 baud
        outb(port + 1, 0x00);  // divisor high byte
        outb(port + 3, 0x03);  // 8 data bits, no parity, 1 stop bit; DLAB off
        outb(port + 2, 0xC7);  // enable FIFO, clear it, 14-byte trigger level
        outb(port + 4, 0x0B);  // mark data-terminal-ready, request-to-send
    }

    inline bool transmit_ready() {
        return inb(port + 5) & 0x20; 
    }

    inline void print(char c) {
        while (!transmit_ready()) {
            // spin until the UART's transmit buffer has room
        }
        outb(port, c);
    }

    inline void print(const char* str) {
        while (*str) {
            print(*str++);
        }
    }

}