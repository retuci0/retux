#include "hex.hpp"
#include "idt.hpp"
#include "pmm.hpp"
#include "serial.hpp"
#include "tss.hpp"
#include "vga.hpp"


// `extern "C"` disables C++ name mangling
extern "C" void kernel_main(u64 multiboot_info_addr) {
    idt::init();
    tss::init();
    pmm::init(multiboot_info_addr);
 
    vga::clear();
    vga::print("hello, world!\n");
 
    serial::init();
    serial::print('\n');
    serial::print("hello, world! (again)\n");
 
    pmm::print_stats();
 
    // smoke test
    char buf[17];
    u64 a = pmm::alloc_frame();
    u64 b = pmm::alloc_frame();
    u64 c = pmm::alloc_frame();
    serial::print("alloc: 0x"); hex::to_string(a, buf); serial::print(buf); serial::print('\n');
    serial::print("alloc: 0x"); hex::to_string(b, buf); serial::print(buf); serial::print('\n');
    serial::print("alloc: 0x"); hex::to_string(c, buf); serial::print(buf); serial::print('\n');
    pmm::free_frame(b);
    u64 d = pmm::alloc_frame();  // should reuse b's frame
    serial::print("after free b, next alloc: 0x"); hex::to_string(d, buf); serial::print(buf); serial::print('\n');
    pmm::free_frame(a);
    pmm::free_frame(c);
    pmm::free_frame(d);
 
    pmm::print_stats();
 
    while (true) {
        asm volatile("hlt");
    }
}
