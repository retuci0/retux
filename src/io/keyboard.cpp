#include "io/keyboard.hpp"
#include "cpu/irq.hpp"
#include "cpu/apic.hpp"
#include "tty/tty.hpp"

#include "lib/types.hpp"
#include "io/serial.hpp"


namespace {

    constexpr int BUFFER_SIZE = 64;
    volatile char buffer[BUFFER_SIZE];
    volatile int head = 0, tail = 0;

    bool alt_pressed = false;
    bool ctrl_pressed = false;
    bool shift_pressed = false;
    bool caps_lock = false;

    // scancode set 1 -> ASCII (just lowercase for now)
    const char scancode_to_ascii[128] = {
        0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0, 0,
        'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 0, 0,
        'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
        'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ',
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    };

    void push_char(char c) {
        if (!c) return;
        int next = (head + 1) % BUFFER_SIZE;
        if (next != tail) {
            buffer[head] = c;
            head = next;
        }
    }

    void keyboard_handler(irq::Frame*) {
        u8 scancode = 0;
        asm volatile("inb $0x60, %0" : "=a"(scancode));

        bool released = scancode & 0x80;
        u8 key = scancode & 0x7F;

        // track shift key (0x2A, 0x36)
        if (key == 0x2A || key == 0x36) {
            shift_pressed = !released;
            return;
        }
        // track caps lock (0x3A)
        if (key == 0x3A) {
            caps_lock = !caps_lock;
            return;
        }
        // track alt key (0x38)
        if (key == 0x38) {
            alt_pressed = !released;
            return;
        }
        // track ctrl (0x1D)
        if (key == 0x1D) {
            ctrl_pressed = !released;
            return;
        }

        // if released, ignore
        if (released) return;

        if (key == 0x0E) {
            push_char('\b');   // TTY will interpret this
            return;
        }

        if (key == 0x1C) {
            push_char('\n');
            return;
        }

        // alt+F1..F4: switch TTY
        if (alt_pressed) {
            if (key >= 0x3B && key <= 0x3E) {  // F1..F4
                int vt = key - 0x3B;  // 0..3
                tty::switch_to(vt);
                return;
            }
        }

        char ascii = scancode_to_ascii[key];
        if (!ascii) return;

        // uppercase handling
        if (ascii >= 'a' && ascii <= 'z') {
            bool uppercase = shift_pressed ^ caps_lock;
            if (uppercase) {
                ascii = ascii - 'a' + 'A';
            }
        }

        push_char(ascii);
    }

}


namespace keyboard {

    void init() {
        irq::register_handler(1, keyboard_handler);
        apic::set_irq_mask(1, false);
        serial::print("keyboard: IRQ1 enabled\n");
    }

    char getchar() {
        if (head == tail) return 0;
        char c = buffer[tail];
        tail = (tail + 1) % BUFFER_SIZE;
        return c;
    }

}
