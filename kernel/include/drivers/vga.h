#ifndef VGA_H
#define VGA_H

#include <stddef.h>
#include <stdint.h>

#define VGA_ADDRESS 0xB8000  // address where vga buffer is stored
#define VGA_WIDTH 80  // 80 collumns wide
#define VGA_HEIGHT 25  // 25 (not 24) rows tall

enum COLOR {
	COLOR_BLACK = 0,
	COLOR_BLUE = 1,
	COLOR_GREEN = 2,
	COLOR_CYAN = 3,
	COLOR_RED = 4,
	COLOR_MAGENTA = 5,
	COLOR_BROWN = 6,
	COLOR_LIGHT_GREY = 7,
	COLOR_DARK_GREY = 8,
	COLOR_LIGHT_BLUE = 9,
	COLOR_LIGHT_GREEN = 10,
	COLOR_LIGHT_CYAN = 11,
	COLOR_LIGHT_RED = 12,
	COLOR_LIGHT_MAGENTA = 13,
	COLOR_LIGHT_BROWN = 14,
	COLOR_WHITE = 15,
};

void set_color(int bg, int fg);
uint16_t vga_entry(char c, uint8_t color);
void write_string(const char *str);
void update_cursor(size_t offset);
void write_char(char c);
void clear_screen(void);
void panic_mode(void);

#endif  // VGA_H
