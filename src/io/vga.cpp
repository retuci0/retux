#include "io/vga.hpp"


void vga::clear() {
    for (int i = 0; i < WIDTH * HEIGHT; ++i)
        buffer[i] = (static_cast<u16>(COLOR) << 8) | ' ';
    cursor_row = 0;
    cursor_col = 0;
}

void vga::print(char c) {
    if (c == '\n') {
        cursor_col = 0;
        if (++cursor_row >= HEIGHT) cursor_row = HEIGHT - 1;
        return;
    }
    if (cursor_row >= HEIGHT || cursor_col >= WIDTH) return;
    int pos = cursor_row * WIDTH + cursor_col;
    buffer[pos] = (static_cast<u16>(COLOR) << 8) | static_cast<u8>(c);
    if (++cursor_col >= WIDTH) {
        cursor_col = 0;
        if (++cursor_row >= HEIGHT) cursor_row = HEIGHT - 1;
    }
}

void vga::print(const char* str) {
    while (*str) {
        print(*str++);
    }
}
