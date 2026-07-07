#pragma once

#include "lib/types.hpp"
#include "lib/port.hpp"


namespace serial {

    constexpr u16 port = 0x3F8;  // COM1

    void init();

    void print(char c);
    void print(const char* str);

    inline bool transmit_ready() {
        return port::inb(port + 5) & 0x20;
    }

    // print a decimal integer (u64)
    inline void print_dec(u64 value) {
        if (value == 0) { print('0'); return; }
        char buf[21]; int i = 20; buf[i] = '\0';
        while (value > 0) { buf[--i] = '0' + static_cast<char>(value % 10); value /= 10; }
        print(buf + i);
    }

}
