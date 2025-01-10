#include "drivers/vga.h"
#include "drivers/io.h"
#include "panic/panic.h"

volatile uint16_t *vga_buffer = (uint16_t *) VGA_ADDRESS;
static size_t offset = 0; // cursor pos
uint8_t color = (COLOR_BLACK << 4) | COLOR_WHITE;

/* store the character and its color in a 16 bit int */
uint16_t vga_entry(char c, uint8_t color) 
{
    return (uint16_t) c | (uint16_t) color << 8;
}

/* change the color settings */
void set_color(int bg, int fg)
{
    color = (bg << 4) | fg;
}

/* write a string to the vga buffer */
void write_string(const char *str) 
{
    for (size_t i = 0; str[i] != '\0'; i++) {
        write_char(str[i]);
    }
}

/* update the position of the cursor*/
void update_cursor(size_t offset) 
{
    uint16_t position = (uint16_t)offset;

    // send the high byte of the position
    outb(0x3D4, 0x0E);
    outb(0x3D5, (position >> 8) & 0xFF);

    // send the low byte of the position
    outb(0x3D4, 0x0F);
    outb(0x3D5, position & 0xFF);
}

/* write a single character to the vga buffer */
void write_char(char c) 
{
    // newline
    if (c == '\n') {
        offset += VGA_WIDTH - (offset % VGA_WIDTH);

    // backspace
    } else if (c == '\b') {
        if (offset > 0) {
            offset--;
            vga_buffer[offset] = vga_entry(' ', color);
        }

    // "tab" (totally not 4 spaces) (please don't airstrike me)
    } else if (c == '\t') {
        int i;
        for (i = 0; i < 4; i++) 
            write_char(' ');

    // regular characters
    } else {
        vga_buffer[offset++] = vga_entry(c, color);

        if (offset >= VGA_WIDTH * VGA_HEIGHT) {
            offset = 0;
        }
    }

    update_cursor(offset);
}

/* clears the screen (duh) */
void clear_screen(void) 
{
    uint8_t color = (COLOR_BLACK << 4) | COLOR_WHITE;
    for (size_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        vga_buffer[i] = vga_entry(' ', color);
    }
    offset = 0;
    update_cursor(offset);
}

/* enter panic mode */
void panic_mode(void)
{
    set_color(COLOR_RED, COLOR_BLACK);
}