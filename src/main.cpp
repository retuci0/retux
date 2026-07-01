#include "hex.hpp"
#include "idt.hpp"
#include "tss.hpp"
#include "serial.hpp"
#include "vga.hpp"


// `extern "C"` disables C++ name mangling
extern "C" void kernel_main(u64 multiboot_info_addr) {
    idt::init();
    tss::init();

    vga::clear();
    vga::print("hello, world!\n");

    serial::init();
    serial::print('\n');
    serial::print("hello, world! (again)\n");
    serial::print("multiboot info struct at 0x");
    {
        char buf[17];
        hex::to_string(multiboot_info_addr, buf);
        serial::print(buf);
    }
    serial::print("\n");

    while (true) {
        asm volatile("hlt");
    }
}