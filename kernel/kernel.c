#include <stddef.h>
#include <stdint.h>

#define VGA_ADDRESS 0xB8000  // memory address of the vga buffer where chars and colors are stored
#define VGA_WIDTH 80  // 80 columns wide
#define VGA_HEIGHT 25  // 25 rows tall

// multiboot specification thing or idk for grub to work
// see: https://en.wikipedia.org/wiki/Multiboot_specification
__attribute__((section(".multiboot")))
const uint32_t multiboot_header[] = {
	0x1BADB002,  // grub magic number
	0x0, // flags
	-(0x1BADB002)  // checksum
};

// colors
enum vga_color {
	COLOR_BLACK = 0,
	COLOR_WHITE = 15,
};


// please don't launch a drone strike to my location for putting the * adjacent to the variable name
// i'm just sticking to linux kernel coding style, since, well, i'm making a kernel lol

/* combine a character and its color into a 16 bit value */
uint16_t vga_entry(char c, uint8_t color) {
	return (uint16_t) c | (uint16_t) color << 8;
}

/* write a string to the vga buffer */
void write_string(const char* str) {
	volatile uint16_t *vga_buffer = (uint16_t *) VGA_ADDRESS;
	uint8_t color = (COLOR_BLACK << 4) | COLOR_WHITE;

	size_t offset = 0;
	for (size_t i = 0; str[i] != '\0'; i++) {
		vga_buffer[offset++] = vga_entry(str[i], color);
	}
}

/* entry point */
void kernel_main(void) {
    write_string("uhhhh hi i guess");  // greet the user
    while (1) {}
}