#include "../include/drivers/keyboard.h"
#include "../include/drivers/vga.h"
#include "../include/multiboot.h"

void kernel_main(void) 
{
    clear_screen();
    write_string("kernel initialized succesfully! type something below:\n");

    while (1) {
        uint8_t scancode = keyboard_read();
        char c = keyboard_handle_scancode(scancode);

        if (c) write_char(c);
    }
}
