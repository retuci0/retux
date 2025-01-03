#include "keyboard.h"
#include "io.h"

static int shift_pressed = 0;
static int caps_lock = 0;

/* read from the keyboard */
uint8_t keyboard_read(void) 
{
    while (!(inb(KEYBOARD_STATUS_PORT) & KEYBOARD_BUFFER_FULL)) {
        // wait until the keyboard buffer is full
    }
    return inb(KEYBOARD_PORT);
}

/* 
 * translate from scancode to ascii character (or action, like caps lock) 
 * it detects (or at least, tries to detect) key releases by detecting "break" key codes,
 * which are the "make" (press) codes of a key, plus 0x80. it currently doesn't work tho.
 * it handles backspaces and enter too.
 * '?' acts as a 'unknown' or invisible character, since it won't show up.
 * this includes the '?' produced when pressing SHIFT + '/', so doing this will result in
 * no character showing up.
 */
char scancode_to_char(uint8_t scancode) 
{
    if (scancode & 0x80) { // key releases
        // currently doesn't detect any key releases at all for some reason
        if (scancode == 0xAA || scancode == 0xB6) {
            shift_pressed = 0;
            // return '?';
        }
    }

    // caps lock
    if (scancode == 0x3A) {
        caps_lock = !caps_lock;
        return '?';
    }
    
    // shift was pressed
    if (scancode == 0x2A || scancode == 0x36) {
        shift_pressed = 1;
        return '?';
    }

    switch (scancode) { // this took me a while
        case 0x29: return (shift_pressed) ? '~' : '`';

        case 0x02: return (shift_pressed) ? '!' : '1';
        case 0x03: return (shift_pressed) ? '"' : '2';
        case 0x04: return (shift_pressed) ? '#' : '3';
        case 0x05: return (shift_pressed) ? '$' : '4';
        case 0x06: return (shift_pressed) ? '%' : '5';
        case 0x07: return (shift_pressed) ? '&' : '6';
        case 0x08: return (shift_pressed) ? '\'' : '7';
        case 0x09: return (shift_pressed) ? '(' : '8';
        case 0x0A: return (shift_pressed) ? ')' : '9';
        case 0x0B: return (shift_pressed) ? '*' : '0';
        case 0x0C: return (shift_pressed) ? '_' : '-';
        case 0x0D: return (shift_pressed) ? '+' : '=';

        case 0x10: return (shift_pressed || caps_lock) ? 'Q' : 'q';
        case 0x11: return (shift_pressed || caps_lock) ? 'W' : 'w';
        case 0x12: return (shift_pressed || caps_lock) ? 'E' : 'e';
        case 0x13: return (shift_pressed || caps_lock) ? 'R' : 'r';
        case 0x14: return (shift_pressed || caps_lock) ? 'T' : 't';
        case 0x15: return (shift_pressed || caps_lock) ? 'Y' : 'y';
        case 0x16: return (shift_pressed || caps_lock) ? 'U' : 'u';
        case 0x17: return (shift_pressed || caps_lock) ? 'I' : 'i';
        case 0x18: return (shift_pressed || caps_lock) ? 'O' : 'o';
        case 0x19: return (shift_pressed || caps_lock) ? 'P' : 'p';

        case 0x1A: return (shift_pressed) ? '{' : '[';
        case 0x1B: return (shift_pressed) ? '}' : ']';
        case 0x1D: return (shift_pressed) ? '|' : '\\';

        case 0x1E: return (shift_pressed || caps_lock) ? 'A' : 'a';
        case 0x1F: return (shift_pressed || caps_lock) ? 'S' : 's';
        case 0x20: return (shift_pressed || caps_lock) ? 'D' : 'd';
        case 0x21: return (shift_pressed || caps_lock) ? 'F' : 'f';
        case 0x22: return (shift_pressed || caps_lock) ? 'G' : 'g';
        case 0x23: return (shift_pressed || caps_lock) ? 'H' : 'h';
        case 0x24: return (shift_pressed || caps_lock) ? 'J' : 'j';
        case 0x25: return (shift_pressed || caps_lock) ? 'K' : 'k';
        case 0x26: return (shift_pressed || caps_lock) ? 'L' : 'l';

        case 0x27: return (shift_pressed) ? ':' : ';';
        case 0x28: return (shift_pressed) ? '"' : '\'';

        case 0x2C: return (shift_pressed || caps_lock) ? 'Z' : 'z';
        case 0x2D: return (shift_pressed || caps_lock) ? 'X' : 'x';
        case 0x2E: return (shift_pressed || caps_lock) ? 'C' : 'c';
        case 0x2F: return (shift_pressed || caps_lock) ? 'V' : 'v';
        case 0x30: return (shift_pressed || caps_lock) ? 'B' : 'b';
        case 0x31: return (shift_pressed || caps_lock) ? 'N' : 'n';
        case 0x32: return (shift_pressed || caps_lock) ? 'M' : 'm';

        case 0x33: return (shift_pressed) ? '<' : ',';
        case 0x34: return (shift_pressed) ? '>' : '.';
        case 0x35: return (shift_pressed) ? '?' : '/';

        case 0x0E: return '\b'; // backspace
        case 0x39: return ' ';  // space
        case 0x1C: return '\n';  // enter
        default: return '?';  // unknown key
    }
}