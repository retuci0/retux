#pragma once

#include "types.h"


// 0xB8000 is the fixed physical address of the VGA text buffer on any PC-compatible machine
// each character cell is 2 bytes: ASCII byte + color

namespace vga {

    volatile u16* const buffer = reinterpret_cast<u16*>(0xB8000);
    constexpr int width = 80;
    constexpr int height = 25;
    constexpr u8 color = 0x0F;  // white text, black bg

    inline int cursor_row = 0;
    inline int cursor_col = 0;

    inline void clear() {
        for (int i = 0; i < width * height; ++i) {
            buffer[i] = (static_cast<u16>(color) << 8) | ' ';
        }
    }

    inline void print(char c) {
        if (c == '\n') {
            cursor_col = 0;
            ++cursor_row;
            return;
        }
        
        const int pos = cursor_row * width + cursor_col;
        buffer[pos] = (static_cast<u16>(color) << 8) | static_cast<u8>(c);
        ++cursor_col;
        if (cursor_col >= width) {
            cursor_col = 0;
            ++cursor_row;
        }
    }

    inline void print(const char* str) {
        while (*str) {
            print(*str++);
        }
    }

}