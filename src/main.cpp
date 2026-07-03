#include "idt.hpp"
#include "tss.hpp"

#include "serial.hpp"
#include "vga.hpp"

#include "pmm.hpp"
#include "vmm.hpp"
#include "heap.hpp"


// `extern "C"` disables C++ name mangling
extern "C" void kernel_main(u64 multiboot_info_addr) {
    serial::init();
    serial::print("\n");

    idt::init(); serial::print("initialized IDT!\n");
    tss::init(); serial::print("initialized TSS!\n");
    
    pmm::init(multiboot_info_addr);
    serial::print("initialized PMM!\n");
    vmm::remap_kernel();
    serial::print("remapped kernel!\n");
    heap::init();
    
    vga::clear();
    vga::print("hello, world!\n");
 
    serial::print('\n');
    serial::print("hello, world! (but better)\n");

    while (true) {
        asm volatile("hlt");
    }
}
