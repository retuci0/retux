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

}
