#ifndef SCANCODE_TRANSLATION_H
#define SCANCODE_TRANSLATION_H

#include <stdint.h>

char translate_scancode(uint8_t scancode, int shift_pressed, int caps_lock);

#endif
