#include "drivers/io.h"

/* assembly one-liner for input byte (read from i/o port) */
uint8_t inb(uint16_t port) 
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* assembly one-liner for output byte (write to i/o port) */
void outb(uint16_t port, uint8_t value) 
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}
