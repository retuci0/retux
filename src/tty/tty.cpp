#include "tty/tty.hpp"

#include "lib/port.hpp"
#include "lib/types.hpp"


namespace {

    constexpr u8  COLOR    = 0x0F;
    constexpr u32 VGA_ADDR = 0xB8000;

    tty::Console consoles[tty::NUM_CONSOLES];
    int active_console = 0;
    bool initialized = false;

    void update_cursor() {
        const auto& con = consoles[active_console];
        int pos = con.row * tty::VGA_WIDTH + con.col;

        port::outb(0x3D4, 0x0F);
        port::outb(0x3D5, pos & 0xFF);
        port::outb(0x3D4, 0x0E);
        port::outb(0x3D5, (pos >> 8) & 0xFF);
    }

    void render_console(int idx) {
        const auto& con = consoles[idx];
        volatile u16* vga = reinterpret_cast<volatile u16*>(VGA_ADDR);
        for (int i = 0; i < tty::VGA_WIDTH * tty::VGA_HEIGHT; ++i) {
            vga[i] = con.buffer[i];
        }
        if (idx == active_console) update_cursor();
    }

    void scroll(tty::Console& con) {
        for (int row = 1; row < tty::VGA_HEIGHT; ++row) {
            int src = row * tty::VGA_WIDTH;
            int dst = (row - 1) * tty::VGA_WIDTH;
            for (int col = 0; col < tty::VGA_WIDTH; ++col) {
                con.buffer[dst + col] = con.buffer[src + col];
            }
        }
        int last = (tty::VGA_HEIGHT - 1) * tty::VGA_WIDTH;
        u16 blank = (static_cast<u16>(COLOR) << 8) | ' ';
        for (int col = 0; col < tty::VGA_WIDTH; ++col) {
            con.buffer[last + col] = blank;
        }
        con.row = tty::VGA_HEIGHT - 1;
        con.col = 0;
    }

}


namespace tty {

    void init() {
        // initialise all consoles
        for (int i = 0; i < NUM_CONSOLES; ++i) {
            auto& con = consoles[i];
            u16 blank = (static_cast<u16>(COLOR) << 8) | ' ';
            for (int j = 0; j < VGA_WIDTH * VGA_HEIGHT; ++j) {
                con.buffer[j] = blank;
            }
            con.row = 0;
            con.col = 0;
        }
        // write labels
        consoles[0].buffer[0] = (static_cast<u16>(COLOR) << 8) | '1';
        consoles[1].buffer[0] = (static_cast<u16>(COLOR) << 8) | '2';
        consoles[2].buffer[0] = (static_cast<u16>(COLOR) << 8) | '3';
        consoles[3].buffer[0] = (static_cast<u16>(COLOR) << 8) | '4';

        active_console = 0;
        render_console(0);
        initialized = true;
    }

    void print(char c) {
        if (!initialized) return;
        auto& con = consoles[active_console];

        // handle newline
        if (c == '\b') {
            if (con.col == 0) {
                con.row--;
                if (con.row < 0) con.row = 0;
                con.col = VGA_WIDTH - 1;
                return;
            }
            con.col--;
            int pos = con.row * VGA_WIDTH + con.col;
            u16 blank = (static_cast<u16>(COLOR) << 8) | ' ';
            con.buffer[pos] = blank;
            volatile u16* vga = reinterpret_cast<volatile u16*>(VGA_ADDR);
            vga[pos] = blank;
            update_cursor();
            return;
        }
        if (c == '\n') {
            con.col = 0;
            if (++con.row >= VGA_HEIGHT) {
                scroll(con);
                render_console(active_console);
                update_cursor();
            } else {
                update_cursor();
            }
            return;
        }

        // build the VGA cell
        u16 cell = (static_cast<u16>(COLOR) << 8) | static_cast<u8>(c);
        int pos = con.row * VGA_WIDTH + con.col;

        // update buffer
        con.buffer[pos] = cell;

        // direct VGA write
        volatile u16* vga = reinterpret_cast<volatile u16*>(VGA_ADDR);
        vga[pos] = cell;

        // advance cursor
        if (++con.col >= VGA_WIDTH) {
            con.col = 0;
            if (++con.row >= VGA_HEIGHT) {
                scroll(con);
                render_console(active_console);
                update_cursor();
                return;
            }
        }
        update_cursor();
    }

    void print(const char* s) {
        while (*s) print(*s++);
    }

    void switch_to(u8 n) {
        if (n < 0 || n >= NUM_CONSOLES) return;
        active_console = n;
        render_console(n);
    }

    u8 active() {
        return active_console;
    }

}
