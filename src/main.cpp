#include "serial.h"
#include "vga.h"


namespace {

    void print_hex64(u64 value) {
        constexpr char digits[] = "0123456789ABCDEF";
        char buf[17];
        buf[16] = '\0';
        for (int i = 15; i >= 0; --i) {
            buf[i] = digits[value & 0xF];
            value >>= 4;
        }
        serial::print(buf);
    }

}

// `extern "C"` disables C++ name mangling
extern "C" void kernel_main(u64 multiboot_info_addr) {
    vga::clear();
    vga::print("hello, world!\n");

    serial::init();
    serial::print('\n');
    serial::print("hello, world! (again)\n");
    serial::print("multiboot info structure at 0x");
    print_hex64(multiboot_info_addr);
    serial::print("\n");

    while (true) {
        asm volatile("hlt");
    }
}