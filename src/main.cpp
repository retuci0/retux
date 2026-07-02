#include "hex.hpp"

#include "idt.hpp"
#include "tss.hpp"

#include "serial.hpp"
#include "vga.hpp"

#include "pmm.hpp"
#include "vmm.hpp"


// `extern "C"` disables C++ name mangling
extern "C" void kernel_main(u64 multiboot_info_addr) {
    serial::init();

    idt::init();
    tss::init();
    
    pmm::init(multiboot_info_addr);
    vmm::remap_kernel();
    
    vga::clear();
    vga::print("hello, world!\n");
 
    serial::print('\n');
    serial::print("hello, world! (again)\n");

    pmm::print_stats();
 
    /* --- smoke test, use to test vmm and pmm --- */
    // char buf[17];
    // constexpr u64 TEST_VIRT = 0x200000;  // 2MB, just above the kernel's 1MB load address range
 
    // u64 frame = pmm::alloc_frame();
    // serial::print("test frame phys:  0x"); hex::to_string(frame, buf); serial::print(buf); serial::print('\n');
 
    // vmm::map(TEST_VIRT, frame, vmm::KERNEL_RW);
 
    // // confirm virt_to_phys gives back the same frame
    // u64 resolved = vmm::virt_to_phys(TEST_VIRT);
    // serial::print("virt_to_phys:     0x"); hex::to_string(resolved, buf); serial::print(buf); serial::print('\n');
    // serial::print("match: "); serial::print(resolved == frame ? "yes\n" : "NO (bug)\n");
 
    // // write and read back through the virtual address
    // auto* ptr = reinterpret_cast<u64*>(TEST_VIRT);
    // *ptr = 0xDEADBEEFCAFEBABEULL;
    // u64 readback = *ptr;
    // serial::print("write/read:       0x"); hex::to_string(readback, buf); serial::print(buf); serial::print('\n');
    // serial::print("correct: "); serial::print(readback == 0xDEADBEEFCAFEBABEULL ? "yes\n" : "NO (bug)\n");
 
    // vmm::unmap(TEST_VIRT);
    // pmm::free_frame(frame);
 
    
    while (true) {
        asm volatile("hlt");
    }
}
 
