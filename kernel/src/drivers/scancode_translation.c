#include "drivers/scancode_translation.h"

// for us international layout
static const char scancode_to_char_table[] = {
    0, '`', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,  'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',  0,
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,  '*', 0,  ' ', 0
};

/* return the desired character based on the key scancode */
char translate_scancode(uint8_t scancode, int shift_pressed, int caps_lock) {
    if (scancode >= sizeof(scancode_to_char_table)) 
        return 0;

    char base_char = scancode_to_char_table[scancode];
    if (!base_char) 
        return 0;

    // apply shift and caps lock modifiers
    if ((shift_pressed || caps_lock) && base_char >= 'a' && base_char <= 'z') {
        return base_char - 32; // convert to uppercase

    // the symbols over the number keys
    } else if (shift_pressed && base_char >= '1' && base_char <= '9') {
        static const char shift_map[] = "!@#$%^&*(";
        return shift_map[base_char - '1'];

    // handle shift for other symbol keys
    } else if (shift_pressed) {
        switch (base_char) {
            case '`': return '~';
            case '-': return '_';
            case '=': return '+';
            case '[': return '{';
            case ']': return '}';
            case '\\': return '|';
            case ';': return ':';
            case '\'': return '"';
            case ',': return '<';
            case '.': return '>';
            case '/': return '?';
            case '0': return ')';
        }
    }
    
    return base_char;
}
