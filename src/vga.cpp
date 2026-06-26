#include "vga.hpp"


void vga::clear() {
    for (int i = 0; i < WIDTH * HEIGHT; ++i) {
        buffer[i] = (static_cast<u16>(COLOR) << 8) | ' ';
    }
}

void vga::print(char c) {
    if (c == '\n') {
        cursor_col = 0;
        ++cursor_row;
        return;
    }
    
    const int pos = cursor_row * WIDTH + cursor_col;
    buffer[pos] = (static_cast<u16>(COLOR) << 8) | static_cast<u8>(c);
    ++cursor_col;
    if (cursor_col >= WIDTH) {
        cursor_col = 0;
        ++cursor_row;
    }
}

void vga::print(const char* str) {
    while (*str) {
        print(*str++);
    }
}