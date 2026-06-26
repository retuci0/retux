#pragma once

#include "types.hpp"


namespace serial {

    constexpr u16 port = 0x3F8;  // COM1

    void outb(u16 p, u8 val);
    u8 inb(u16 p);

    void init();

    void print(char c);
    void print(const char* str);
    
    inline bool transmit_ready() {
        return inb(port + 5) & 0x20; 
    }

}