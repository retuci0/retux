#include "vga.h"
#include "io.h"

volatile uint16_t *vga_buffer = (uint16_t *) VGA_ADDRESS;
static size_t offset = 0; // cursor pos

/* store the character and its color in a 16 bit int */
uint16_t vga_entry(char c, uint8_t color) 
{
    return (uint16_t) c | (uint16_t) color << 8;
}

/* write a string to the vga buffer */
void write_string(const char *str) 
{
    uint8_t color = (COLOR_BLACK << 4) | COLOR_WHITE;

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
    uint8_t color = (COLOR_BLACK << 4) | COLOR_WHITE;

    if (c == '\n') {
        offset += VGA_WIDTH - (offset % VGA_WIDTH);
    } else if (c == '\b') {
        if (offset > 0) {
            offset--;
            vga_buffer[offset] = vga_entry(' ', color);
        }
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
