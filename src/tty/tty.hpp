#pragma once

#include "lib/types.hpp"


namespace tty {

    constexpr u8 VGA_WIDTH  = 80;
    constexpr u8 VGA_HEIGHT = 25;
    constexpr u8 NUM_CONSOLES = 4;

    struct Console {
        u16 buffer[VGA_WIDTH * VGA_HEIGHT];
        u8 row;
        u8 col;
    };

    void init();

    // write a single character to the active console.
    void print(char c);
    void print(const char* s);

    // switch to console `n` (0‑based). updates VGA and hardware cursor.
    void switch_to(u8 n);

    // return the active console index.
    u8 active();

}
