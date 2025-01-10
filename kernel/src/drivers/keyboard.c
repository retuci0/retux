#include "drivers/keyboard.h"
#include "drivers/io.h"
#include "drivers/scancode_translation.h"

static int shift_pressed = 0;
static int caps_lock = 0;

/* handle modifiers like the shift and the caps lock keys */
static void keyboard_update_state(uint8_t scancode) 
{
    if (scancode == 0x2A || scancode == 0x36) // press shift keys
        shift_pressed = 1;
    else if (scancode == 0xAA || scancode == 0xB6) // release shift keys
        shift_pressed = 0;
    else if (scancode == 0x3A) // caps lock toggle
        caps_lock = !caps_lock;
}

/* read from the keyboard with the inb function */
uint8_t keyboard_read(void) {
    while (!(inb(KEYBOARD_STATUS_PORT) & KEYBOARD_BUFFER_FULL)) {
        // wait until the keyboard buffer is full
    }
    return inb(KEYBOARD_PORT);
}

/* as the name indicates, handle the key scancode properly */
char keyboard_handle_scancode(uint8_t scancode) {
    if (scancode & 0x80) {  // break code (key release)
        keyboard_update_state(scancode);
        return 0;
    } else {
        keyboard_update_state(scancode);
        return translate_scancode(scancode, shift_pressed, caps_lock);
    }
}
