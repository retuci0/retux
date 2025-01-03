#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

#define KEYBOARD_PORT 0x60  // data port
#define KEYBOARD_STATUS_PORT 0x64 // status port
#define KEYBOARD_BUFFER_FULL 0x01 // bitmask to check if buffer is full

uint8_t keyboard_read(void);
char scancode_to_char(uint8_t scancode);

#endif  // KEYBOARD_H
