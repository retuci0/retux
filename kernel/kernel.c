#include "vga.h"
#include "keyboard.h"
#include "multiboot.h"

/* entry point */
void kernel_main(void) 
{
    write_string("uhhhh hi i guess\n");

    while (1) {
        uint8_t scancode = keyboard_read();
        if (scancode < 0x80) {
            char c = scancode_to_char(scancode);
            if (c != '?') {
                write_char(c);
            }
        }
    }
}