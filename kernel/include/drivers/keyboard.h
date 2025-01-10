#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

#define KEYBOARD_PORT 0x60
#define KEYBOARD_STATUS_PORT 0x64
#define KEYBOARD_BUFFER_FULL 0x01

uint8_t keyboard_read(void);
char keyboard_handle_scancode(uint8_t scancode);

#endif
