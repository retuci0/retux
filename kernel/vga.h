#ifndef VGA_H
#define VGA_H

#include <stddef.h>
#include <stdint.h>

#define VGA_ADDRESS 0xB8000  // address where vga buffer is stored
#define VGA_WIDTH 80  // 80 collumns wide
#define VGA_HEIGHT 25  // 25 (not 24) rows tall

enum vga_color {
    COLOR_BLACK = 0,
    COLOR_WHITE = 15,
};

uint16_t vga_entry(char c, uint8_t color);
void write_string(const char *str);
void write_char(char c);

#endif  // VGA_H
