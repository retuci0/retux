#include "drivers/vga.h"
#include "drivers/io.h"
#include "panic/panic.h"

void kernel_panic(const char *message, const char *file, int line)
{
    clear_screen();
    panic_mode();
    write_string("KERNEL PANICKED\n");
    
    write_string("message: ");
    write_string(message);
    write_string("\nfile: ");
    write_string(file);
    // write_string("\nline: ");
    // write_int(line);  // no function to display ints but who cares right it's not like it's gonna panic or sum

    write_string("\nsystem halted.\n");

    while (1) 
        __asm__ volatile ("hlt");
}