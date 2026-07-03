#pragma once

#include "lib/types.hpp"


// 0xB8000 is the fixed physical address of the VGA text buffer on any PC-compatible machine
// each character cell is 2 bytes: ASCII byte + color

namespace vga {

    volatile u16* const buffer = reinterpret_cast<u16*>(0xB8000);
    constexpr int WIDTH  = 80;
    constexpr int HEIGHT = 25;
    constexpr u8  COLOR  = 0x0F;  // white text, black bg

    inline int cursor_row = 0;
    inline int cursor_col = 0;

    void clear();

    void print(char c);
    void print(const char* str);
}
